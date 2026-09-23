#include <gtest/gtest.h>

#include <algorithm>

#include <QString>

#include "Search.h"

using namespace noteAppC;

namespace {

/// Builds a NoteContext with no tags/headings/tasks - most tests only need
/// the text.
NoteContext note(const QString &text, const QString &path = QStringLiteral("Note.md"))
{
    NoteContext n;
    n.path = path;
    n.text = text;
    return n;
}

/// Runs `query` against `text` and returns just the matched offsets, sorted,
/// which is what most of these tests want to assert on.
QList<qsizetype> offsetsOf(const QString &query, const QString &text)
{
    const SearchQuery q = SearchQuery::parse(query);
    EXPECT_TRUE(q.isValid()) << q.error().toStdString();
    const SearchOutcome outcome = q.evaluate(note(text));
    QList<qsizetype> offsets;
    for (const SearchMatch &m : outcome.matches)
        offsets << m.offset;
    std::sort(offsets.begin(), offsets.end());
    return offsets;
}

bool matches(const QString &query, const NoteContext &n)
{
    const SearchQuery q = SearchQuery::parse(query);
    EXPECT_TRUE(q.isValid()) << q.error().toStdString();
    return q.evaluate(n).matched;
}

bool matchesText(const QString &query, const QString &text)
{
    return matches(query, note(text));
}

} // namespace

// --- bare words: implicit AND ----------------------------------------------

TEST(SearchGrammarTest, BareWordsAreCaseInsensitiveSubstringMatches)
{
    EXPECT_TRUE(matchesText(QStringLiteral("meeting"), QStringLiteral("A Meeting happened")));
    EXPECT_FALSE(matchesText(QStringLiteral("bogus"), QStringLiteral("A Meeting happened")));
}

TEST(SearchGrammarTest, SeveralBareWordsAreAllRequired)
{
    EXPECT_TRUE(matchesText(QStringLiteral("meeting work"), QStringLiteral("meeting about work today")));
    EXPECT_FALSE(matchesText(QStringLiteral("meeting golf"), QStringLiteral("meeting about work today")));
}

TEST(SearchGrammarTest, EveryOccurrenceIsReported)
{
    const QList<qsizetype> offsets = offsetsOf(QStringLiteral("cat"), QStringLiteral("cat sat, cat ran"));
    EXPECT_EQ(offsets, QList<qsizetype>({0, 9}));
}

// --- "quoted phrases" -------------------------------------------------------

TEST(SearchGrammarTest, QuotedPhraseMatchesExactAdjacentWords)
{
    EXPECT_TRUE(matchesText(QStringLiteral("\"star wars\""), QStringLiteral("I love star wars a lot")));
    EXPECT_FALSE(matchesText(QStringLiteral("\"wars star\""), QStringLiteral("I love star wars a lot")));
}

TEST(SearchGrammarTest, QuotedPhraseSupportsEscapedQuotes)
{
    EXPECT_TRUE(matchesText(QStringLiteral(R"("they said \"hello\"")"),
                            QStringLiteral("they said \"hello\" to me")));
}

// --- OR ----------------------------------------------------------------

TEST(SearchGrammarTest, OrMatchesEitherSide)
{
    EXPECT_TRUE(matchesText(QStringLiteral("meeting OR work"), QStringLiteral("just a work day")));
    EXPECT_TRUE(matchesText(QStringLiteral("meeting OR work"), QStringLiteral("a meeting today")));
    EXPECT_FALSE(matchesText(QStringLiteral("meeting OR golf"), QStringLiteral("a work day")));
}

TEST(SearchGrammarTest, LowercaseOrIsJustAWord)
{
    // Obsidian requires OR in capitals - "or" is an ordinary bare word and
    // joins the implicit AND instead of introducing a union.
    EXPECT_TRUE(matchesText(QStringLiteral("cats or dogs"), QStringLiteral("cats or dogs and mice")));
    EXPECT_FALSE(matchesText(QStringLiteral("cats or dogs"), QStringLiteral("cats and dogs")));
}

TEST(SearchGrammarTest, JuxtaposedAndOrBindTheWayTheDocsExampleShows)
{
    // "meeting work OR meetup personal" == (meeting AND work) OR (meetup AND personal)
    const QString query = QStringLiteral("meeting work OR meetup personal");
    EXPECT_TRUE(matchesText(query, QStringLiteral("a meeting about work")));
    EXPECT_TRUE(matchesText(query, QStringLiteral("a personal meetup")));
    EXPECT_FALSE(matchesText(query, QStringLiteral("a meeting alone")));
    EXPECT_FALSE(matchesText(query, QStringLiteral("a personal meeting"))); // wrong pairing
}

// --- -negation ---------------------------------------------------------

TEST(SearchGrammarTest, NegationExcludesTheTerm)
{
    EXPECT_TRUE(matchesText(QStringLiteral("meeting -golf"), QStringLiteral("a meeting about work")));
    EXPECT_FALSE(matchesText(QStringLiteral("meeting -work"), QStringLiteral("a meeting about work")));
}

TEST(SearchGrammarTest, NegationContributesNoHighlight)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("meeting -golf"));
    ASSERT_TRUE(q.isValid());
    const SearchOutcome outcome = q.evaluate(note(QStringLiteral("a meeting about work")));
    ASSERT_TRUE(outcome.matched);
    // Only "meeting" should have a location; "-golf" never matched anything.
    EXPECT_EQ(outcome.matches.size(), 1);
}

// --- (parentheses) -------------------------------------------------------

TEST(SearchGrammarTest, GroupingChangesPrecedence)
{
    // "meeting (work OR meetup) personal" - both work/meetup AND personal.
    const QString query = QStringLiteral("meeting (work OR meetup) personal");
    EXPECT_TRUE(matchesText(query, QStringLiteral("meeting, work, personal")));
    EXPECT_TRUE(matchesText(query, QStringLiteral("meeting, meetup, personal")));
    EXPECT_FALSE(matchesText(query, QStringLiteral("meeting, work"))); // no "personal"
}

TEST(SearchGrammarTest, NegatedGroupExcludesEitherTerm)
{
    // "meeting -(work meetup)" excludes notes that have BOTH work and meetup.
    const QString query = QStringLiteral("meeting -(work meetup)");
    EXPECT_TRUE(matchesText(query, QStringLiteral("meeting about work only")));
    EXPECT_FALSE(matchesText(query, QStringLiteral("meeting about work and a meetup")));
}

// --- fields: file: path: content: -------------------------------------

TEST(SearchGrammarTest, FileMatchesTheFileName)
{
    NoteContext n = note(QStringLiteral("body text"), QStringLiteral("Projects/plan.jpg.md"));
    EXPECT_TRUE(matches(QStringLiteral("file:.jpg"), n));
    EXPECT_FALSE(matches(QStringLiteral("file:.png"), n));
}

TEST(SearchGrammarTest, PathMatchesTheWholePath)
{
    NoteContext n = note(QStringLiteral("body text"), QStringLiteral("Daily notes/2022-07-01.md"));
    EXPECT_TRUE(matches(QStringLiteral("path:\"Daily notes/2022-07\""), n));
    EXPECT_FALSE(matches(QStringLiteral("path:\"Daily notes/2022-08\""), n));
}

TEST(SearchGrammarTest, ContentIsTransparent)
{
    EXPECT_TRUE(matchesText(QStringLiteral("content:\"happy cat\""), QStringLiteral("a happy cat sat")));
}

// --- tag: ----------------------------------------------------------------

TEST(SearchGrammarTest, TagMatchesByNameAndNestedChild)
{
    NoteContext n = note(QStringLiteral("text #work/meeting text"));
    n.tags.append(NoteTag{QStringLiteral("work/meeting"), Range{5, 18}});
    EXPECT_TRUE(matches(QStringLiteral("tag:#work"), n));
    EXPECT_TRUE(matches(QStringLiteral("tag:work/meeting"), n));
    EXPECT_FALSE(matches(QStringLiteral("tag:play"), n));
}

TEST(SearchGrammarTest, AFrontmatterTagHighlightsItsOwnWordNotTheBlock)
{
    // The index gives a frontmatter tag its property block as its range.
    const QString text = QStringLiteral("---\ntags: [a, work]\n---\nbody\n");
    NoteContext n = note(text);
    n.tags.append(NoteTag{QStringLiteral("a"), Range{0, 24}});
    n.tags.append(NoteTag{QStringLiteral("work"), Range{0, 24}});
    const SearchQuery work = SearchQuery::parse(QStringLiteral("tag:#work"));
    const SearchOutcome found = work.evaluate(n);
    ASSERT_EQ(found.matches.size(), 1);
    EXPECT_EQ(text.mid(found.matches.first().offset, found.matches.first().length), QStringLiteral("work"));
    // Whole words: "a" is not the a in "tags".
    const SearchOutcome single = SearchQuery::parse(QStringLiteral("tag:a")).evaluate(n);
    ASSERT_EQ(single.matches.size(), 1);
    EXPECT_EQ(single.matches.first().offset, text.indexOf(QStringLiteral("[a")) + 1);
    EXPECT_EQ(single.matches.first().length, 1);
}

// --- line:(...) block:(...) section:(...) -------------------------------

TEST(SearchGrammarTest, LineRequiresBothWordsOnTheSameLine)
{
    const QString text = QStringLiteral("mix the flour\nadd sugar\n");
    EXPECT_TRUE(matchesText(QStringLiteral("line:(mix flour)"), text));
    EXPECT_FALSE(matchesText(QStringLiteral("line:(mix sugar)"), text));
}

TEST(SearchGrammarTest, BlockRequiresBothWordsInTheSameParagraph)
{
    const QString text = QStringLiteral("dog\ncat\n\nfish\n"); // two paragraphs
    EXPECT_TRUE(matchesText(QStringLiteral("block:(dog cat)"), text));
    EXPECT_FALSE(matchesText(QStringLiteral("block:(dog fish)"), text));
}

TEST(SearchGrammarTest, SectionRequiresBothWordsUnderTheSameHeading)
{
    NoteContext n = note(QStringLiteral("# One\ndog here\n# Two\ncat here\n"));
    n.headings.append(NoteHeading{1, Range{0, 5}});
    n.headings.append(NoteHeading{1, Range{15, 20}});
    EXPECT_FALSE(matches(QStringLiteral("section:(dog cat)"), n));
    EXPECT_TRUE(matches(QStringLiteral("section:(dog here)"), n));
}

TEST(SearchGrammarTest, NoHeadingsMeansOneWholeDocumentSection)
{
    EXPECT_TRUE(matchesText(QStringLiteral("section:(dog cat)"), QStringLiteral("dog\n\ncat\n")));
}

// --- task: task-todo: task-done: ----------------------------------------

TEST(SearchGrammarTest, TaskFieldsRespectDoneState)
{
    const QString text = QStringLiteral("- [ ] call mum\n- [x] call plumber\n");
    NoteContext n = note(text);
    n.tasks.append(NoteTask{false, Range{0, 14}});
    n.tasks.append(NoteTask{true, Range{15, 34}});

    EXPECT_TRUE(matches(QStringLiteral("task:call"), n));
    EXPECT_TRUE(matches(QStringLiteral("task-todo:mum"), n));
    EXPECT_FALSE(matches(QStringLiteral("task-todo:plumber"), n));
    EXPECT_TRUE(matches(QStringLiteral("task-done:plumber"), n));
    EXPECT_FALSE(matches(QStringLiteral("task-done:mum"), n));
}

// --- match-case: ignore-case: -------------------------------------------

TEST(SearchGrammarTest, MatchCaseIsExact)
{
    EXPECT_TRUE(matchesText(QStringLiteral("match-case:HappyCat"), QStringLiteral("a HappyCat sat")));
    EXPECT_FALSE(matchesText(QStringLiteral("match-case:HappyCat"), QStringLiteral("a happycat sat")));
}

TEST(SearchGrammarTest, IgnoreCaseOverridesAGlobalMatchCase)
{
    // Both terms default to case-insensitive already; ignore-case: just has
    // to keep matching when asked to, which is the operator's whole contract.
    EXPECT_TRUE(matchesText(QStringLiteral("ignore-case:ikea"), QStringLiteral("built by IKEA")));
}

// --- /regular expressions/ ------------------------------------------------

TEST(SearchGrammarTest, RegexMatchesAndReportsOffsets)
{
    const QList<qsizetype> offsets =
        offsetsOf(QStringLiteral("/\\d{4}-\\d{2}-\\d{2}/"), QStringLiteral("seen on 2024-01-05 and 2024-02-09"));
    EXPECT_EQ(offsets, QList<qsizetype>({8, 23}));
}

TEST(SearchGrammarTest, RegexCombinesWithAField)
{
    NoteContext n = note(QStringLiteral("body"), QStringLiteral("Daily/2024-01-05.md"));
    EXPECT_TRUE(matches(QStringLiteral("path:/\\d{4}-\\d{2}-\\d{2}/"), n));
    EXPECT_FALSE(matches(QStringLiteral("path:/nope/"), n));
}

// --- UTF-16 offsets --------------------------------------------------------

TEST(SearchGrammarTest, OffsetsAreUtf16CodeUnitsAcrossASurrogatePair)
{
    // U+1F600 is a surrogate pair - two UTF-16 code units - so "target" here
    // sits two code units later than its plain character count would suggest.
    const QString text = QStringLiteral("\U0001F600target");
    const QList<qsizetype> offsets = offsetsOf(QStringLiteral("target"), text);
    ASSERT_EQ(offsets.size(), 1);
    EXPECT_EQ(offsets.first(), 2);
    EXPECT_EQ(text.mid(offsets.first(), 6), QStringLiteral("target"));
}

// --- parse errors ----------------------------------------------------------

TEST(SearchGrammarTest, UnterminatedQuoteIsAParseError)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("\"never closed"));
    EXPECT_FALSE(q.isValid());
    EXPECT_FALSE(q.error().isEmpty());
}

TEST(SearchGrammarTest, UnmatchedParenIsAParseError)
{
    EXPECT_FALSE(SearchQuery::parse(QStringLiteral("(open")).isValid());
    EXPECT_FALSE(SearchQuery::parse(QStringLiteral("close)")).isValid());
}

TEST(SearchGrammarTest, UnterminatedRegexIsAParseError)
{
    EXPECT_FALSE(SearchQuery::parse(QStringLiteral("/never closed")).isValid());
}

TEST(SearchGrammarTest, UnknownFieldNameIsJustAWord)
{
    // "foo" is not a recognised field, so "foo:bar" is one literal token -
    // the same way a URL survives being typed into the search box.
    EXPECT_TRUE(matchesText(QStringLiteral("foo:bar"), QStringLiteral("see foo:bar here")));
}

// --- FTS5 narrowing (the optimisation, checked so it never lies) ----------

TEST(SearchFtsTranslationTest, PlainWordsBecomeAnAndExpression)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("meeting work"));
    ASSERT_TRUE(q.isValid());
    EXPECT_EQ(q.ftsExpression(), QStringLiteral("\"meeting\" AND \"work\""));
}

TEST(SearchFtsTranslationTest, NegationAloneIsWild)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("-work"));
    ASSERT_TRUE(q.isValid());
    EXPECT_TRUE(q.ftsExpression().isEmpty());
}

TEST(SearchFtsTranslationTest, NegationInsideAndStillNarrowsOnTheOtherTerm)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("meeting -work"));
    ASSERT_TRUE(q.isValid());
    EXPECT_EQ(q.ftsExpression(), QStringLiteral("\"meeting\""));
}

TEST(SearchFtsTranslationTest, OrWithAnUntranslatableBranchIsWild)
{
    // A bare regex cannot be expressed in FTS5, so the whole OR must stay
    // wild - narrowing on "meeting" alone would wrongly drop notes that only
    // satisfy the regex side.
    const SearchQuery q = SearchQuery::parse(QStringLiteral("meeting OR /[0-9]+/"));
    ASSERT_TRUE(q.isValid());
    EXPECT_TRUE(q.ftsExpression().isEmpty());
}

TEST(SearchFtsTranslationTest, ScopedFieldsStillNarrowOnPresence)
{
    // line:(mix flour) requires both words *somewhere* in the document even
    // though evaluate() will insist they share a line.
    const SearchQuery q = SearchQuery::parse(QStringLiteral("line:(mix flour)"));
    ASSERT_TRUE(q.isValid());
    EXPECT_EQ(q.ftsExpression(), QStringLiteral("\"mix\" AND \"flour\""));
}

TEST(SearchFtsTranslationTest, PathOnlyQueryIsWild)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("path:foo"));
    ASSERT_TRUE(q.isValid());
    EXPECT_TRUE(q.ftsExpression().isEmpty());
}

// --- FieldNeeds (the other optimisation) ----------------------------------

TEST(SearchFieldNeedsTest, PlainWordsNeedNothingExtra)
{
    const SearchQuery q = SearchQuery::parse(QStringLiteral("meeting work"));
    ASSERT_TRUE(q.isValid());
    const FieldNeeds needs = q.fieldNeeds();
    EXPECT_FALSE(needs.tags);
    EXPECT_FALSE(needs.headings);
    EXPECT_FALSE(needs.tasks);
}

TEST(SearchFieldNeedsTest, EachFieldFlagsItsOwnTable)
{
    EXPECT_TRUE(SearchQuery::parse(QStringLiteral("tag:work")).fieldNeeds().tags);
    EXPECT_TRUE(SearchQuery::parse(QStringLiteral("section:(a b)")).fieldNeeds().headings);
    EXPECT_TRUE(SearchQuery::parse(QStringLiteral("task-todo:x")).fieldNeeds().tasks);
    EXPECT_TRUE(SearchQuery::parse(QStringLiteral("line:(tag:work)")).fieldNeeds().tags);
}

// --- snippets ----------------------------------------------------------

TEST(SearchSnippetTest, CentresOnTheFirstMatchAndMarksTruncation)
{
    QString text = QStringLiteral("word ");
    while (text.size() < 200)
        text += QStringLiteral("filler ");
    text += QStringLiteral("TARGET");
    text += QStringLiteral(" more filler after it to pad the tail out nicely");

    const qsizetype targetOffset = text.indexOf(QStringLiteral("TARGET"));
    const QList<SearchMatch> matches{{targetOffset, 6}};
    const SearchSnippet snippet = buildSnippet(text, matches, 20);
    EXPECT_TRUE(snippet.text.contains(QStringLiteral("TARGET")));
    EXPECT_TRUE(snippet.text.startsWith(QStringLiteral("…")));
    EXPECT_TRUE(snippet.text.endsWith(QStringLiteral("…")));

    ASSERT_EQ(snippet.highlights.size(), 1);
    const SearchMatch &highlight = snippet.highlights.first();
    EXPECT_EQ(snippet.text.mid(highlight.offset, highlight.length), QStringLiteral("TARGET"));
}

TEST(SearchSnippetTest, FallsBackToTheStartWhenThereIsNoPositionedMatch)
{
    // A path:/file: only match has no offset into the body.
    const SearchSnippet snippet = buildSnippet(QStringLiteral("the very start of the note"), {}, 10);
    EXPECT_TRUE(snippet.text.startsWith(QStringLiteral("the very")));
    EXPECT_TRUE(snippet.highlights.isEmpty());
}

TEST(SearchSnippetTest, HighlightsEveryMatchThatFallsInsideTheWindow)
{
    const QString text = QStringLiteral("cat sat near the cat mat");
    QList<SearchMatch> matches;
    qsizetype from = 0;
    while (true) {
        const qsizetype hit = text.indexOf(QStringLiteral("cat"), from);
        if (hit < 0)
            break;
        matches.append({hit, 3});
        from = hit + 3;
    }
    ASSERT_EQ(matches.size(), 2);

    const SearchSnippet snippet = buildSnippet(text, matches, 60); // window covers the whole text
    ASSERT_EQ(snippet.highlights.size(), 2);
    for (const SearchMatch &highlight : snippet.highlights)
        EXPECT_EQ(snippet.text.mid(highlight.offset, highlight.length), QStringLiteral("cat"));
}
