#ifndef regex_impl_hh_INCLUDED
#define regex_impl_hh_INCLUDED

#include "unicode.hh"
#include "utf8.hh"
#include "utf8_iterator.hh"
#include "vector.hh"
#include "flags.hh"

namespace Kakoune
{

struct CompiledRegex
{
    enum Op : char
    {
        Match,
        Literal,
        LiteralIgnoreCase,
        AnyChar,
        Matcher,
        Jump,
        Split_PrioritizeParent,
        Split_PrioritizeChild,
        Save,
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        SubjectBegin,
        SubjectEnd,
        LookAhead,
        LookBehind,
        NegativeLookAhead,
        NegativeLookBehind,
    };

    using Offset = unsigned;
    static constexpr Offset search_prefix_size = 3 + 2 * sizeof(Offset);

    explicit operator bool() const { return not bytecode.empty(); }

    Vector<char> bytecode;
    Vector<std::function<bool (Codepoint)>> matchers;
    size_t save_count;
};

CompiledRegex compile_regex(StringView re);

enum class RegexExecFlags
{
    None              = 0,
    Search            = 1 << 0,
    NotBeginOfLine    = 1 << 1,
    NotEndOfLine      = 1 << 2,
    NotBeginOfWord    = 1 << 3,
    NotEndOfWord      = 1 << 4,
    NotBeginOfSubject = 1 << 5,
    NotInitialNull    = 1 << 6,
    AnyMatch          = 1 << 7
};

constexpr bool with_bit_ops(Meta::Type<RegexExecFlags>) { return true; }

template<typename Iterator>
struct ThreadedRegexVM
{
    ThreadedRegexVM(const CompiledRegex& program)
      : m_program{program} { kak_assert(m_program); }

    struct Saves
    {
        int refcount;
        Vector<Iterator> pos;
    };

    Saves* clone_saves(Saves* saves)
    {
        if (not m_free_saves.empty())
        {
            Saves* res = m_free_saves.back();
            m_free_saves.pop_back();
            res->refcount = 1;
            res->pos = saves->pos;
            return res;
        }

        m_saves.push_back(std::make_unique<Saves>(Saves{1, saves->pos}));
        return m_saves.back().get();
    }

    struct Thread
    {
        const char* inst;
        Saves* saves;
    };

    enum class StepResult { Consumed, Matched, Failed };
    StepResult step(Thread& thread)
    {
        const auto prog_start = m_program.bytecode.data();
        const auto prog_end = prog_start + m_program.bytecode.size();
        while (true)
        {
            const Codepoint cp = m_pos == m_end ? 0 : *m_pos;
            const CompiledRegex::Op op = (CompiledRegex::Op)*thread.inst++;
            switch (op)
            {
                case CompiledRegex::Literal:
                    if (utf8::read_codepoint(thread.inst, prog_end) == cp)
                        return StepResult::Consumed;
                    return StepResult::Failed;
                case CompiledRegex::LiteralIgnoreCase:
                    if (utf8::read_codepoint(thread.inst, prog_end) == to_lower(cp))
                        return StepResult::Consumed;
                    return StepResult::Failed;
                case CompiledRegex::AnyChar:
                    return StepResult::Consumed;
                case CompiledRegex::Jump:
                    thread.inst = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst);
                    break;
                case CompiledRegex::Split_PrioritizeParent:
                {
                    auto parent = thread.inst + sizeof(CompiledRegex::Offset);
                    auto child = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst);
                    thread.inst = parent;
                    ++thread.saves->refcount;
                    m_current_threads.push_back({child, thread.saves});
                    break;
                }
                case CompiledRegex::Split_PrioritizeChild:
                {
                    auto parent = thread.inst + sizeof(CompiledRegex::Offset);
                    auto child = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst);
                    thread.inst = child;
                    ++thread.saves->refcount;
                    m_current_threads.push_back({parent, thread.saves});
                    break;
                }
                case CompiledRegex::Save:
                {
                    const char index = *thread.inst++;
                    if (thread.saves->refcount > 1)
                    {
                        --thread.saves->refcount;
                        thread.saves = clone_saves(thread.saves);
                    }
                    thread.saves->pos[index] = m_pos.base();
                    break;
                }
                case CompiledRegex::Matcher:
                {
                    const int matcher_id = *thread.inst++;
                    return m_program.matchers[matcher_id](*m_pos) ?
                        StepResult::Consumed : StepResult::Failed;
                }
                case CompiledRegex::LineStart:
                    if (not is_line_start())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::LineEnd:
                    if (not is_line_end())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::WordBoundary:
                    if (not is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::NotWordBoundary:
                    if (is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectBegin:
                    if (m_pos != m_begin or m_flags & RegexExecFlags::NotBeginOfSubject)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectEnd:
                    if (m_pos != m_end)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::LookAhead:
                case CompiledRegex::NegativeLookAhead:
                {
                    int count = *thread.inst++;
                    for (auto it = m_pos; count and it != m_end; ++it, --count)
                        if (*it != utf8::read(thread.inst))
                            break;
                    if ((op == CompiledRegex::LookAhead and count != 0) or
                        (op == CompiledRegex::NegativeLookAhead and count == 0))
                        return StepResult::Failed;
                    thread.inst = utf8::advance(thread.inst, prog_end, CharCount{count - 1});
                    break;
                }
                case CompiledRegex::LookBehind:
                case CompiledRegex::NegativeLookBehind:
                {
                    int count = *thread.inst++;
                    for (auto it = m_pos-1; count and it >= m_begin; --it, --count)
                        if (*it != utf8::read(thread.inst))
                            break;
                    if ((op == CompiledRegex::LookBehind and count != 0) or
                        (op == CompiledRegex::NegativeLookBehind and count == 0))
                        return StepResult::Failed;
                    thread.inst = utf8::advance(thread.inst, prog_end, CharCount{count - 1});
                    break;
                }
                case CompiledRegex::Match:
                    return StepResult::Matched;
            }
        }
        return StepResult::Failed;
    }

    bool exec(Iterator begin, Iterator end, RegexExecFlags flags)
    {
        bool found_match = false;
        m_current_threads.clear();
        m_next_threads.clear();

        const auto start_offset = (flags & RegexExecFlags::Search) ? 0 : CompiledRegex::search_prefix_size;
        m_saves.push_back(std::make_unique<Saves>(Saves{1, Vector<Iterator>(m_program.save_count, Iterator{})}));
        m_current_threads.push_back({m_program.bytecode.data() + start_offset, m_saves.back().get()});

        m_begin = begin;
        m_end = end;
        m_flags = flags;

        if (flags & RegexExecFlags::NotInitialNull and m_begin == m_end)
            return false;

        auto release_saves = [this](Saves* saves) {
            if (--saves->refcount == 0)
                m_free_saves.push_back(saves);
        };

        for (m_pos = Utf8It{m_begin, m_begin, m_end}; m_pos != m_end; ++m_pos)
        {
            while (not m_current_threads.empty())
            {
                auto thread = m_current_threads.back();
                m_current_threads.pop_back();
                switch (step(thread))
                {
                case StepResult::Matched:
                    if (not (flags & RegexExecFlags::Search) or // We are not at end, this is not a full match
                        (flags & RegexExecFlags::NotInitialNull and m_pos == m_begin))
                    {
                        release_saves(thread.saves);
                        continue;
                    }

                    m_captures = std::move(thread.saves->pos);
                    if (flags & RegexExecFlags::AnyMatch)
                        return true;

                    found_match = true;
                    m_current_threads.clear(); // remove this and lower priority threads
                    break;
                case StepResult::Failed:
                    release_saves(thread.saves);
                    break;
                case StepResult::Consumed:
                    if (contains_that(m_next_threads, [&](auto& t) { return t.inst == thread.inst; }))
                        release_saves(thread.saves);
                    else
                        m_next_threads.push_back(thread);
                    break;
                }
            }
            if (m_next_threads.empty())
                return found_match;

            std::swap(m_current_threads, m_next_threads);
            std::reverse(m_current_threads.begin(), m_current_threads.end());
        }
        if (found_match)
            return true;

        // Step remaining threads to see if they match without consuming anything else
        while (not m_current_threads.empty())
        {
            auto thread = m_current_threads.back();
            m_current_threads.pop_back();
            if (step(thread) == StepResult::Matched)
            {
                m_captures = std::move(thread.saves->pos);
                return true;
            }
        }
        return false;
    }

    bool is_line_start() const
    {
        return (m_pos == m_begin and not (m_flags & RegexExecFlags::NotBeginOfLine)) or
               *(m_pos-1) == '\n';
    }

    bool is_line_end() const
    {
        return (m_pos == m_end and not (m_flags & RegexExecFlags::NotEndOfLine)) or
               *m_pos == '\n';
    }

    bool is_word_boundary() const
    {
        return (m_pos == m_begin and not (m_flags & RegexExecFlags::NotBeginOfWord)) or
               (m_pos == m_end and not (m_flags & RegexExecFlags::NotEndOfWord)) or
               is_word(*(m_pos-1)) != is_word(*m_pos);
    }

    const CompiledRegex& m_program;
    Vector<Thread> m_current_threads;
    Vector<Thread> m_next_threads;

    using Utf8It = utf8::iterator<Iterator>;

    Iterator m_begin;
    Iterator m_end;
    Utf8It m_pos;
    RegexExecFlags m_flags;

    Vector<std::unique_ptr<Saves>> m_saves;
    Vector<Saves*> m_free_saves;

    Vector<Iterator> m_captures;
};

template<typename It>
bool regex_match(It begin, It end, const CompiledRegex& re, RegexExecFlags flags = RegexExecFlags::None)
{
    ThreadedRegexVM<It> vm{re};
    return vm.exec(begin, end, (RegexExecFlags)(flags & ~(RegexExecFlags::Search)) | RegexExecFlags::AnyMatch);
}

template<typename It>
bool regex_match(It begin, It end, Vector<It>& captures, const CompiledRegex& re,
                 RegexExecFlags flags = RegexExecFlags::None)
{
    ThreadedRegexVM<It> vm{re};
    if (vm.exec(begin, end,  flags & ~(RegexExecFlags::Search)))
    {
        captures = std::move(vm.m_captures);
        return true;
    }
    return false;
}

template<typename It>
bool regex_search(It begin, It end, const CompiledRegex& re,
                  RegexExecFlags flags = RegexExecFlags::None)
{
    ThreadedRegexVM<It> vm{re};
    return vm.exec(begin, end, flags | RegexExecFlags::Search | RegexExecFlags::AnyMatch);
}

template<typename It>
bool regex_search(It begin, It end, Vector<It>& captures, const CompiledRegex& re,
                  RegexExecFlags flags = RegexExecFlags::None)
{
    ThreadedRegexVM<It> vm{re};
    if (vm.exec(begin, end, flags | RegexExecFlags::Search))
    {
        captures = std::move(vm.m_captures);
        return true;
    }
    return false;
}

}

#endif // regex_impl_hh_INCLUDED