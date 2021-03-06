//===--- Format.cpp - Format C++ code -------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file implements functions declared in Format.h. This will be
/// split into separate files as we go.
///
/// This is EXPERIMENTAL code under heavy development. It is not in a state yet,
/// where it can be used to format real code.
///
//===----------------------------------------------------------------------===//

#include "clang/Format/Format.h"
#include "UnwrappedLineParser.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include <string>

namespace clang {
namespace format {

// FIXME: Move somewhere sane.
struct TokenAnnotation {
  enum TokenType {
    TT_Unknown,
    TT_TemplateOpener,
    TT_TemplateCloser,
    TT_BinaryOperator,
    TT_UnaryOperator,
    TT_OverloadedOperator,
    TT_PointerOrReference,
    TT_ConditionalExpr,
    TT_CtorInitializerColon,
    TT_LineComment,
    TT_BlockComment
  };

  TokenType Type;

  bool SpaceRequiredBefore;
  bool CanBreakBefore;
  bool MustBreakBefore;
};

using llvm::MutableArrayRef;

FormatStyle getLLVMStyle() {
  FormatStyle LLVMStyle;
  LLVMStyle.ColumnLimit = 80;
  LLVMStyle.MaxEmptyLinesToKeep = 1;
  LLVMStyle.PointerAndReferenceBindToType = false;
  LLVMStyle.AccessModifierOffset = -2;
  LLVMStyle.SplitTemplateClosingGreater = true;
  LLVMStyle.IndentCaseLabels = false;
  return LLVMStyle;
}

FormatStyle getGoogleStyle() {
  FormatStyle GoogleStyle;
  GoogleStyle.ColumnLimit = 80;
  GoogleStyle.MaxEmptyLinesToKeep = 1;
  GoogleStyle.PointerAndReferenceBindToType = true;
  GoogleStyle.AccessModifierOffset = -1;
  GoogleStyle.SplitTemplateClosingGreater = false;
  GoogleStyle.IndentCaseLabels = true;
  return GoogleStyle;
}

struct OptimizationParameters {
  unsigned PenaltyIndentLevel;
};

class UnwrappedLineFormatter {
public:
  UnwrappedLineFormatter(const FormatStyle &Style, SourceManager &SourceMgr,
                         const UnwrappedLine &Line,
                         const std::vector<TokenAnnotation> &Annotations,
                         tooling::Replacements &Replaces, bool StructuralError)
      : Style(Style), SourceMgr(SourceMgr), Line(Line),
        Annotations(Annotations), Replaces(Replaces),
        StructuralError(StructuralError) {
    Parameters.PenaltyIndentLevel = 5;
  }

  void format() {
    // Format first token and initialize indent.
    unsigned Indent = formatFirstToken();

    // Initialize state dependent on indent.
    IndentState State;
    State.Column = Indent;
    State.ConsumedTokens = 0;
    State.Indent.push_back(Indent + 4);
    State.LastSpace.push_back(Indent);
    State.FirstLessLess.push_back(0);

    // The first token has already been indented and thus consumed.
    moveStateToNextToken(State);

    // Check whether the UnwrappedLine can be put onto a single line. If so,
    // this is bound to be the optimal solution (by definition) and we don't
    // need to analyze the entire solution space. 
    unsigned Columns = State.Column;
    bool FitsOnALine = true;
    for (unsigned i = 1, n = Line.Tokens.size(); i != n; ++i) {
      Columns += (Annotations[i].SpaceRequiredBefore ? 1 : 0) +
          Line.Tokens[i].Tok.getLength();
      // A special case for the colon of a constructor initializer as this only
      // needs to be put on a new line if the line needs to be split.
      if (Columns > Style.ColumnLimit ||
          (Annotations[i].MustBreakBefore &&
           Annotations[i].Type != TokenAnnotation::TT_CtorInitializerColon)) {
        FitsOnALine = false;
        break;
      }
    }

    // Start iterating at 1 as we have correctly formatted of Token #0 above.
    for (unsigned i = 1, n = Line.Tokens.size(); i != n; ++i) {
      if (FitsOnALine) {
        addTokenToState(false, false, State);
      } else {
        unsigned NoBreak = calcPenalty(State, false, UINT_MAX);
        unsigned Break = calcPenalty(State, true, NoBreak);
        addTokenToState(Break < NoBreak, false, State);
      }
    }
  }

private:
  /// \brief The current state when indenting a unwrapped line.
  ///
  /// As the indenting tries different combinations this is copied by value.
  struct IndentState {
    /// \brief The number of used columns in the current line.
    unsigned Column;

    /// \brief The number of tokens already consumed.
    unsigned ConsumedTokens;

    /// \brief The position to which a specific parenthesis level needs to be
    /// indented.
    std::vector<unsigned> Indent;

    /// \brief The position of the last space on each level.
    ///
    /// Used e.g. to break like:
    /// functionCall(Parameter, otherCall(
    ///                             OtherParameter));
    std::vector<unsigned> LastSpace;

    /// \brief The position the first "<<" operator encountered on each level.
    ///
    /// Used to align "<<" operators. 0 if no such operator has been encountered
    /// on a level.
    std::vector<unsigned> FirstLessLess;

    /// \brief Comparison operator to be able to used \c IndentState in \c map.
    bool operator<(const IndentState &Other) const {
      if (Other.ConsumedTokens != ConsumedTokens)
        return Other.ConsumedTokens > ConsumedTokens;
      if (Other.Column != Column)
        return Other.Column > Column;
      if (Other.Indent.size() != Indent.size())
        return Other.Indent.size() > Indent.size();
      for (int i = 0, e = Indent.size(); i != e; ++i) {
        if (Other.Indent[i] != Indent[i])
          return Other.Indent[i] > Indent[i];
      }
      if (Other.LastSpace.size() != LastSpace.size())
        return Other.LastSpace.size() > LastSpace.size();
      for (int i = 0, e = LastSpace.size(); i != e; ++i) {
        if (Other.LastSpace[i] != LastSpace[i])
          return Other.LastSpace[i] > LastSpace[i];
      }
      if (Other.FirstLessLess.size() != FirstLessLess.size())
        return Other.FirstLessLess.size() > FirstLessLess.size();
      for (int i = 0, e = FirstLessLess.size(); i != e; ++i) {
        if (Other.FirstLessLess[i] != FirstLessLess[i])
          return Other.FirstLessLess[i] > FirstLessLess[i];
      }
      return false;
    }
  };

  /// \brief Appends the next token to \p State and updates information
  /// necessary for indentation.
  ///
  /// Puts the token on the current line if \p Newline is \c true and adds a
  /// line break and necessary indentation otherwise.
  ///
  /// If \p DryRun is \c false, also creates and stores the required
  /// \c Replacement.
  void addTokenToState(bool Newline, bool DryRun, IndentState &State) {
    unsigned Index = State.ConsumedTokens;
    const FormatToken &Current = Line.Tokens[Index];
    const FormatToken &Previous = Line.Tokens[Index - 1];
    unsigned ParenLevel = State.Indent.size() - 1;

    if (Newline) {
      if (Current.Tok.is(tok::string_literal) &&
          Previous.Tok.is(tok::string_literal))
        State.Column = State.Column - Previous.Tok.getLength();
      else if (Current.Tok.is(tok::lessless) &&
               State.FirstLessLess[ParenLevel] != 0)
        State.Column = State.FirstLessLess[ParenLevel];
      else if (ParenLevel != 0 &&
               (Previous.Tok.is(tok::equal) || Current.Tok.is(tok::arrow) ||
                Current.Tok.is(tok::period)))
        // Indent and extra 4 spaces after '=' as it continues an expression.
        // Don't do that on the top level, as we already indent 4 there.
        State.Column = State.Indent[ParenLevel] + 4;
      else
        State.Column = State.Indent[ParenLevel];

      if (!DryRun)
        replaceWhitespace(Current, 1, State.Column);

      State.LastSpace[ParenLevel] = State.Indent[ParenLevel];
      if (Current.Tok.is(tok::colon) &&
          Annotations[Index].Type != TokenAnnotation::TT_ConditionalExpr)
        State.Indent[ParenLevel] += 2;
    } else {
      unsigned Spaces = Annotations[Index].SpaceRequiredBefore ? 1 : 0;
      if (Annotations[Index].Type == TokenAnnotation::TT_LineComment)
        Spaces = 2;

      if (!DryRun)
        replaceWhitespace(Current, 0, Spaces);

      if (Previous.Tok.is(tok::l_paren) ||
          Annotations[Index - 1].Type == TokenAnnotation::TT_TemplateOpener)
        State.Indent[ParenLevel] = State.Column;

      // Top-level spaces are exempt as that mostly leads to better results.
      State.Column += Spaces;
      if (Spaces > 0 && ParenLevel != 0)
        State.LastSpace[ParenLevel] = State.Column;
    }
    moveStateToNextToken(State);
  }

  /// \brief Mark the next token as consumed in \p State and modify its stacks
  /// accordingly.
  void moveStateToNextToken(IndentState &State) {
    unsigned Index = State.ConsumedTokens;
    const FormatToken &Current = Line.Tokens[Index];
    unsigned ParenLevel = State.Indent.size() - 1;

    if (Current.Tok.is(tok::lessless) && State.FirstLessLess[ParenLevel] == 0)
      State.FirstLessLess[ParenLevel] = State.Column;

    State.Column += Current.Tok.getLength();

    // If we encounter an opening (, [ or <, we add a level to our stacks to
    // prepare for the following tokens.
    if (Current.Tok.is(tok::l_paren) || Current.Tok.is(tok::l_square) ||
        Annotations[Index].Type == TokenAnnotation::TT_TemplateOpener) {
      State.Indent.push_back(4 + State.LastSpace.back());
      State.LastSpace.push_back(State.LastSpace.back());
      State.FirstLessLess.push_back(0);
    }

    // If we encounter a closing ), ] or >, we can remove a level from our
    // stacks.
    if (Current.Tok.is(tok::r_paren) || Current.Tok.is(tok::r_square) ||
        Annotations[Index].Type == TokenAnnotation::TT_TemplateCloser) {
      State.Indent.pop_back();
      State.LastSpace.pop_back();
      State.FirstLessLess.pop_back();
    }

    ++State.ConsumedTokens;
  }

  /// \brief Calculate the panelty for splitting after the token at \p Index.
  unsigned splitPenalty(unsigned Index) {
    assert(Index < Line.Tokens.size() &&
           "Tried to calculate penalty for splitting after the last token");
    const FormatToken &Left = Line.Tokens[Index];
    const FormatToken &Right = Line.Tokens[Index + 1];
    if (Left.Tok.is(tok::semi) || Left.Tok.is(tok::comma))
      return 0;
    if (Left.Tok.is(tok::equal) || Left.Tok.is(tok::l_paren) ||
        Left.Tok.is(tok::pipepipe) || Left.Tok.is(tok::ampamp))
      return 2;

    if (Right.Tok.is(tok::arrow) || Right.Tok.is(tok::period))
      return 200;

    return 3;
  }

  /// \brief Calculate the number of lines needed to format the remaining part
  /// of the unwrapped line.
  ///
  /// Assumes the formatting so far has led to
  /// the \c IndentState \p State. If \p NewLine is set, a new line will be
  /// added after the previous token.
  ///
  /// \param StopAt is used for optimization. If we can determine that we'll
  /// definitely need at least \p StopAt additional lines, we already know of a
  /// better solution.
  unsigned calcPenalty(IndentState State, bool NewLine, unsigned StopAt) {
    // We are at the end of the unwrapped line, so we don't need any more lines.
    if (State.ConsumedTokens >= Line.Tokens.size())
      return 0;

    if (!NewLine && Annotations[State.ConsumedTokens].MustBreakBefore)
      return UINT_MAX;
    if (NewLine && !Annotations[State.ConsumedTokens].CanBreakBefore)
      return UINT_MAX;

    unsigned CurrentPenalty = 0;
    if (NewLine) {
      CurrentPenalty += Parameters.PenaltyIndentLevel * State.Indent.size() +
          splitPenalty(State.ConsumedTokens - 1);
    }

    addTokenToState(NewLine, true, State);

    // Exceeding column limit is bad.
    if (State.Column > Style.ColumnLimit)
      return UINT_MAX;

    if (StopAt <= CurrentPenalty)
      return UINT_MAX;
    StopAt -= CurrentPenalty;

    StateMap::iterator I = Memory.find(State);
    if (I != Memory.end()) {
      // If this state has already been examined, we can safely return the
      // previous result if we
      // - have not hit the optimatization (and thus returned UINT_MAX) OR
      // - are now computing for a smaller or equal StopAt.
      unsigned SavedResult = I->second.first;
      unsigned SavedStopAt = I->second.second;
      if (SavedResult != UINT_MAX)
        return SavedResult + CurrentPenalty;
      else if (StopAt <= SavedStopAt)
        return UINT_MAX;
    }

    unsigned NoBreak = calcPenalty(State, false, StopAt);
    unsigned WithBreak = calcPenalty(State, true, std::min(StopAt, NoBreak));
    unsigned Result = std::min(NoBreak, WithBreak);

    // We have to store 'Result' without adding 'CurrentPenalty' as the latter
    // can depend on 'NewLine'.
    Memory[State] = std::pair<unsigned, unsigned>(Result, StopAt);

    return Result == UINT_MAX ? UINT_MAX : Result + CurrentPenalty;
  }

  /// \brief Replaces the whitespace in front of \p Tok. Only call once for
  /// each \c FormatToken.
  void replaceWhitespace(const FormatToken &Tok, unsigned NewLines,
                         unsigned Spaces) {
    Replaces.insert(tooling::Replacement(
        SourceMgr, Tok.WhiteSpaceStart, Tok.WhiteSpaceLength,
        std::string(NewLines, '\n') + std::string(Spaces, ' ')));
  }

  /// \brief Add a new line and the required indent before the first Token
  /// of the \c UnwrappedLine if there was no structural parsing error.
  /// Returns the indent level of the \c UnwrappedLine.
  unsigned formatFirstToken() {
    const FormatToken &Token = Line.Tokens[0];
    if (!Token.WhiteSpaceStart.isValid() || StructuralError)
      return SourceMgr.getSpellingColumnNumber(Token.Tok.getLocation()) - 1;

    unsigned Newlines =
        std::min(Token.NewlinesBefore, Style.MaxEmptyLinesToKeep + 1);
    unsigned Offset = SourceMgr.getFileOffset(Token.WhiteSpaceStart);
    if (Newlines == 0 && Offset != 0)
      Newlines = 1;
    unsigned Indent = Line.Level * 2;
    if ((Token.Tok.is(tok::kw_public) || Token.Tok.is(tok::kw_protected) ||
         Token.Tok.is(tok::kw_private)) &&
        static_cast<int>(Indent) + Style.AccessModifierOffset >= 0)
      Indent += Style.AccessModifierOffset;
    replaceWhitespace(Token, Newlines, Indent);
    return Indent;
  }

  FormatStyle Style;
  SourceManager &SourceMgr;
  const UnwrappedLine &Line;
  const std::vector<TokenAnnotation> &Annotations;
  tooling::Replacements &Replaces;
  bool StructuralError;

  // A map from an indent state to a pair (Result, Used-StopAt).
  typedef std::map<IndentState, std::pair<unsigned, unsigned> > StateMap;
  StateMap Memory;

  OptimizationParameters Parameters;
};

/// \brief Determines extra information about the tokens comprising an
/// \c UnwrappedLine.
class TokenAnnotator {
public:
  TokenAnnotator(const UnwrappedLine &Line, const FormatStyle &Style,
                 SourceManager &SourceMgr)
      : Line(Line), Style(Style), SourceMgr(SourceMgr) {
  }

  /// \brief A parser that gathers additional information about tokens.
  ///
  /// The \c TokenAnnotator tries to matches parenthesis and square brakets and
  /// store a parenthesis levels. It also tries to resolve matching "<" and ">"
  /// into template parameter lists.
  class AnnotatingParser {
  public:
    AnnotatingParser(const SmallVector<FormatToken, 16> &Tokens,
                     std::vector<TokenAnnotation> &Annotations)
        : Tokens(Tokens), Annotations(Annotations), Index(0) {
    }

    bool parseAngle() {
      while (Index < Tokens.size()) {
        if (Tokens[Index].Tok.is(tok::greater)) {
          Annotations[Index].Type = TokenAnnotation::TT_TemplateCloser;
          next();
          return true;
        }
        if (Tokens[Index].Tok.is(tok::r_paren) ||
            Tokens[Index].Tok.is(tok::r_square))
          return false;
        if (Tokens[Index].Tok.is(tok::pipepipe) ||
            Tokens[Index].Tok.is(tok::ampamp) ||
            Tokens[Index].Tok.is(tok::question) ||
            Tokens[Index].Tok.is(tok::colon))
          return false;
        consumeToken();
      }
      return false;
    }

    bool parseParens() {
      while (Index < Tokens.size()) {
        if (Tokens[Index].Tok.is(tok::r_paren)) {
          next();
          return true;
        }
        if (Tokens[Index].Tok.is(tok::r_square))
          return false;
        consumeToken();
      }
      return false;
    }

    bool parseSquare() {
      while (Index < Tokens.size()) {
        if (Tokens[Index].Tok.is(tok::r_square)) {
          next();
          return true;
        }
        if (Tokens[Index].Tok.is(tok::r_paren))
          return false;
        consumeToken();
      }
      return false;
    }

    bool parseConditional() {
      while (Index < Tokens.size()) {
        if (Tokens[Index].Tok.is(tok::colon)) {
          Annotations[Index].Type = TokenAnnotation::TT_ConditionalExpr;
          next();
          return true;
        }
        consumeToken();
      }
      return false;
    }

    void consumeToken() {
      unsigned CurrentIndex = Index;
      next();
      switch (Tokens[CurrentIndex].Tok.getKind()) {
      case tok::l_paren:
        parseParens();
        if (Index < Tokens.size() && Tokens[Index].Tok.is(tok::colon)) {
          Annotations[Index].Type = TokenAnnotation::TT_CtorInitializerColon;
          next();
        }
        break;
      case tok::l_square:
        parseSquare();
        break;
      case tok::less:
        if (parseAngle())
          Annotations[CurrentIndex].Type = TokenAnnotation::TT_TemplateOpener;
        else {
          Annotations[CurrentIndex].Type = TokenAnnotation::TT_BinaryOperator;
          Index = CurrentIndex + 1;
        }
        break;
      case tok::greater:
        Annotations[CurrentIndex].Type = TokenAnnotation::TT_BinaryOperator;
        break;
      case tok::kw_operator:
        if (!Tokens[Index].Tok.is(tok::l_paren))
          Annotations[Index].Type = TokenAnnotation::TT_OverloadedOperator;
        next();
        break;
      case tok::question:
        parseConditional();
        break;
      default:
        break;
      }
    }

    void parseLine() {
      while (Index < Tokens.size()) {
        consumeToken();
      }
    }

    void next() {
      ++Index;
    }

  private:
    const SmallVector<FormatToken, 16> &Tokens;
    std::vector<TokenAnnotation> &Annotations;
    unsigned Index;
  };

  void annotate() {
    Annotations.clear();
    for (int i = 0, e = Line.Tokens.size(); i != e; ++i) {
      Annotations.push_back(TokenAnnotation());
    }

    AnnotatingParser Parser(Line.Tokens, Annotations);
    Parser.parseLine();

    determineTokenTypes();

    for (int i = 1, e = Line.Tokens.size(); i != e; ++i) {
      TokenAnnotation &Annotation = Annotations[i];

      Annotation.CanBreakBefore =
          canBreakBetween(Line.Tokens[i - 1], Line.Tokens[i]);

      if (Annotation.Type == TokenAnnotation::TT_CtorInitializerColon) {
        Annotation.MustBreakBefore = true;
        Annotation.SpaceRequiredBefore = true;
      } else if (Line.Tokens[i].Tok.is(tok::colon)) {
        Annotation.SpaceRequiredBefore =
            Line.Tokens[0].Tok.isNot(tok::kw_case) && i != e - 1;
      } else if (Annotations[i - 1].Type == TokenAnnotation::TT_UnaryOperator) {
        Annotation.SpaceRequiredBefore = false;
      } else if (Annotation.Type == TokenAnnotation::TT_UnaryOperator) {
        Annotation.SpaceRequiredBefore =
            Line.Tokens[i - 1].Tok.isNot(tok::l_paren) &&
            Line.Tokens[i - 1].Tok.isNot(tok::l_square);
      } else if (Line.Tokens[i - 1].Tok.is(tok::greater) &&
                 Line.Tokens[i].Tok.is(tok::greater)) {
        if (Annotation.Type == TokenAnnotation::TT_TemplateCloser &&
            Annotations[i - 1].Type == TokenAnnotation::TT_TemplateCloser)
          Annotation.SpaceRequiredBefore = Style.SplitTemplateClosingGreater;
        else
          Annotation.SpaceRequiredBefore = false;
      } else if (
          Annotation.Type == TokenAnnotation::TT_BinaryOperator ||
          Annotations[i - 1].Type == TokenAnnotation::TT_BinaryOperator) {
        Annotation.SpaceRequiredBefore = true;
      } else if (
          Annotations[i - 1].Type == TokenAnnotation::TT_TemplateCloser &&
          Line.Tokens[i].Tok.is(tok::l_paren)) {
        Annotation.SpaceRequiredBefore = false;
      } else if (Line.Tokens[i].Tok.is(tok::less) &&
                 Line.Tokens[0].Tok.is(tok::hash)) {
        Annotation.SpaceRequiredBefore = true;
      } else {
        Annotation.SpaceRequiredBefore =
            spaceRequiredBetween(Line.Tokens[i - 1].Tok, Line.Tokens[i].Tok);
      }

      if (Annotations[i - 1].Type == TokenAnnotation::TT_LineComment ||
          (Line.Tokens[i].Tok.is(tok::string_literal) &&
           Line.Tokens[i - 1].Tok.is(tok::string_literal))) {
        Annotation.MustBreakBefore = true;
      }

      if (Annotation.MustBreakBefore)
        Annotation.CanBreakBefore = true;
    }
  }

  const std::vector<TokenAnnotation> &getAnnotations() {
    return Annotations;
  }

private:
  void determineTokenTypes() {
    bool AssignmentEncountered = false;
    for (int i = 0, e = Line.Tokens.size(); i != e; ++i) {
      TokenAnnotation &Annotation = Annotations[i];
      const FormatToken &Tok = Line.Tokens[i];

      if (Tok.Tok.is(tok::equal) || Tok.Tok.is(tok::plusequal) ||
          Tok.Tok.is(tok::minusequal) || Tok.Tok.is(tok::starequal) ||
          Tok.Tok.is(tok::slashequal))
        AssignmentEncountered = true;

      if (Tok.Tok.is(tok::star) || Tok.Tok.is(tok::amp))
        Annotation.Type = determineStarAmpUsage(i, AssignmentEncountered);
      else if (isUnaryOperator(i))
        Annotation.Type = TokenAnnotation::TT_UnaryOperator;
      else if (isBinaryOperator(Line.Tokens[i]))
        Annotation.Type = TokenAnnotation::TT_BinaryOperator;
      else if (Tok.Tok.is(tok::comment)) {
        StringRef Data(SourceMgr.getCharacterData(Tok.Tok.getLocation()),
                       Tok.Tok.getLength());
        if (Data.startswith("//"))
          Annotation.Type = TokenAnnotation::TT_LineComment;
        else
          Annotation.Type = TokenAnnotation::TT_BlockComment;
      }
    }
  }

  bool isUnaryOperator(unsigned Index) {
    const Token &Tok = Line.Tokens[Index].Tok;

    // '++', '--' and '!' are always unary operators.
    if (Tok.is(tok::minusminus) || Tok.is(tok::plusplus) ||
        Tok.is(tok::exclaim))
      return true;

    // The other possible unary operators are '+' and '-' as we
    // determine the usage of '*' and '&' in determineStarAmpUsage().
    if (Tok.isNot(tok::minus) && Tok.isNot(tok::plus))
      return false;

    // Use heuristics to recognize unary operators.
    const Token &PreviousTok = Line.Tokens[Index - 1].Tok;
    if (PreviousTok.is(tok::equal) || PreviousTok.is(tok::l_paren) ||
        PreviousTok.is(tok::comma) || PreviousTok.is(tok::l_square))
      return true;

    // Fall back to marking the token as binary operator.
    return Annotations[Index - 1].Type == TokenAnnotation::TT_BinaryOperator;
  }

  bool isBinaryOperator(const FormatToken &Tok) {
    switch (Tok.Tok.getKind()) {
    case tok::equal:
    case tok::equalequal:
    case tok::exclaimequal:
    case tok::star:
      //case tok::amp:
    case tok::plus:
    case tok::slash:
    case tok::minus:
    case tok::ampamp:
    case tok::pipe:
    case tok::pipepipe:
    case tok::percent:
      return true;
    default:
      return false;
    }
  }

  TokenAnnotation::TokenType determineStarAmpUsage(unsigned Index,
                                                   bool AssignmentEncountered) {
    if (Index == Annotations.size())
      return TokenAnnotation::TT_Unknown;

    if (Index == 0 || Line.Tokens[Index - 1].Tok.is(tok::l_paren) ||
        Line.Tokens[Index - 1].Tok.is(tok::comma) ||
        Annotations[Index - 1].Type == TokenAnnotation::TT_BinaryOperator)
      return TokenAnnotation::TT_UnaryOperator;

    if (Line.Tokens[Index - 1].Tok.isLiteral() ||
        Line.Tokens[Index + 1].Tok.isLiteral())
      return TokenAnnotation::TT_BinaryOperator;

    // It is very unlikely that we are going to find a pointer or reference type
    // definition on the RHS of an assignment.
    if (AssignmentEncountered)
      return TokenAnnotation::TT_BinaryOperator;

    return TokenAnnotation::TT_PointerOrReference;
  }

  bool isIfForOrWhile(Token Tok) {
    return Tok.is(tok::kw_if) || Tok.is(tok::kw_for) || Tok.is(tok::kw_while);
  }

  bool spaceRequiredBetween(Token Left, Token Right) {
    if (Right.is(tok::r_paren) || Right.is(tok::semi) || Right.is(tok::comma))
      return false;
    if (Left.is(tok::kw_template) && Right.is(tok::less))
      return true;
    if (Left.is(tok::arrow) || Right.is(tok::arrow))
      return false;
    if (Left.is(tok::exclaim) || Left.is(tok::tilde))
      return false;
    if (Left.is(tok::less) || Right.is(tok::greater) || Right.is(tok::less))
      return false;
    if (Right.is(tok::amp) || Right.is(tok::star))
      return Left.isLiteral() ||
          (Left.isNot(tok::star) && Left.isNot(tok::amp) &&
           !Style.PointerAndReferenceBindToType);
    if (Left.is(tok::amp) || Left.is(tok::star))
      return Right.isLiteral() || Style.PointerAndReferenceBindToType;
    if (Right.is(tok::star) && Left.is(tok::l_paren))
      return false;
    if (Left.is(tok::l_square) || Right.is(tok::l_square) ||
        Right.is(tok::r_square))
      return false;
    if (Left.is(tok::coloncolon) ||
        (Right.is(tok::coloncolon) &&
         (Left.is(tok::identifier) || Left.is(tok::greater))))
      return false;
    if (Left.is(tok::period) || Right.is(tok::period))
      return false;
    if (Left.is(tok::colon) || Right.is(tok::colon))
      return true;
    if ((Left.is(tok::plusplus) && Right.isAnyIdentifier()) ||
        (Left.isAnyIdentifier() && Right.is(tok::plusplus)) ||
        (Left.is(tok::minusminus) && Right.isAnyIdentifier()) ||
        (Left.isAnyIdentifier() && Right.is(tok::minusminus)))
      return false;
    if (Left.is(tok::l_paren))
      return false;
    if (Left.is(tok::hash))
      return false;
    if (Right.is(tok::l_paren)) {
      return !Left.isAnyIdentifier() || isIfForOrWhile(Left);
    }
    return true;
  }

  bool canBreakBetween(const FormatToken &Left, const FormatToken &Right) {
    if (Right.Tok.is(tok::r_paren) || Right.Tok.is(tok::l_brace) ||
        Right.Tok.is(tok::comment) || Right.Tok.is(tok::greater))
      return false;
    if (isBinaryOperator(Left) || Right.Tok.is(tok::lessless) ||
        Right.Tok.is(tok::arrow) || Right.Tok.is(tok::period))
      return true;
    return Right.Tok.is(tok::colon) || Left.Tok.is(tok::comma) || Left.Tok.is(
        tok::semi) || Left.Tok.is(tok::equal) || Left.Tok.is(tok::ampamp) ||
        Left.Tok.is(tok::pipepipe) || Left.Tok.is(tok::l_brace) ||
        (Left.Tok.is(tok::l_paren) && !Right.Tok.is(tok::r_paren));
  }

  const UnwrappedLine &Line;
  FormatStyle Style;
  SourceManager &SourceMgr;
  std::vector<TokenAnnotation> Annotations;
};

class LexerBasedFormatTokenSource : public FormatTokenSource {
public:
  LexerBasedFormatTokenSource(Lexer &Lex, SourceManager &SourceMgr)
      : GreaterStashed(false), Lex(Lex), SourceMgr(SourceMgr),
        IdentTable(Lex.getLangOpts()) {
    Lex.SetKeepWhitespaceMode(true);
  }

  virtual FormatToken getNextToken() {
    if (GreaterStashed) {
      FormatTok.NewlinesBefore = 0;
      FormatTok.WhiteSpaceStart =
          FormatTok.Tok.getLocation().getLocWithOffset(1);
      FormatTok.WhiteSpaceLength = 0;
      GreaterStashed = false;
      return FormatTok;
    }

    FormatTok = FormatToken();
    Lex.LexFromRawLexer(FormatTok.Tok);
    FormatTok.WhiteSpaceStart = FormatTok.Tok.getLocation();

    // Consume and record whitespace until we find a significant token.
    while (FormatTok.Tok.is(tok::unknown)) {
      FormatTok.NewlinesBefore += tokenText(FormatTok.Tok).count('\n');
      FormatTok.WhiteSpaceLength += FormatTok.Tok.getLength();

      if (FormatTok.Tok.is(tok::eof))
        return FormatTok;
      Lex.LexFromRawLexer(FormatTok.Tok);
    }

    if (FormatTok.Tok.is(tok::raw_identifier)) {
      const IdentifierInfo &Info = IdentTable.get(tokenText(FormatTok.Tok));
      FormatTok.Tok.setKind(Info.getTokenID());
    }

    if (FormatTok.Tok.is(tok::greatergreater)) {
      FormatTok.Tok.setKind(tok::greater);
      GreaterStashed = true;
    }

    return FormatTok;
  }

private:
  FormatToken FormatTok;
  bool GreaterStashed;
  Lexer &Lex;
  SourceManager &SourceMgr;
  IdentifierTable IdentTable;

  /// Returns the text of \c FormatTok.
  StringRef tokenText(Token &Tok) {
    return StringRef(SourceMgr.getCharacterData(Tok.getLocation()),
                     Tok.getLength());
  }
};

class Formatter : public UnwrappedLineConsumer {
public:
  Formatter(const FormatStyle &Style, Lexer &Lex, SourceManager &SourceMgr,
            const std::vector<CharSourceRange> &Ranges)
      : Style(Style), Lex(Lex), SourceMgr(SourceMgr), Ranges(Ranges),
        StructuralError(false) {
  }

  virtual ~Formatter() {
  }

  tooling::Replacements format() {
    LexerBasedFormatTokenSource Tokens(Lex, SourceMgr);
    UnwrappedLineParser Parser(Style, Tokens, *this);
    StructuralError = Parser.parse();
    for (std::vector<UnwrappedLine>::iterator I = UnwrappedLines.begin(),
                                              E = UnwrappedLines.end();
         I != E; ++I)
      formatUnwrappedLine(*I);
    return Replaces;
  }

private:
  virtual void consumeUnwrappedLine(const UnwrappedLine &TheLine) {
    UnwrappedLines.push_back(TheLine);
  }

  void formatUnwrappedLine(const UnwrappedLine &TheLine) {
    if (TheLine.Tokens.size() == 0)
      return;

    CharSourceRange LineRange =
        CharSourceRange::getTokenRange(TheLine.Tokens.front().Tok.getLocation(),
                                       TheLine.Tokens.back().Tok.getLocation());

    for (unsigned i = 0, e = Ranges.size(); i != e; ++i) {
      if (SourceMgr.isBeforeInTranslationUnit(LineRange.getEnd(),
                                              Ranges[i].getBegin()) ||
          SourceMgr.isBeforeInTranslationUnit(Ranges[i].getEnd(),
                                              LineRange.getBegin()))
        continue;

      TokenAnnotator Annotator(TheLine, Style, SourceMgr);
      Annotator.annotate();
      UnwrappedLineFormatter Formatter(Style, SourceMgr, TheLine,
                                       Annotator.getAnnotations(), Replaces,
                                       StructuralError);
      Formatter.format();
      return;
    }
  }

  FormatStyle Style;
  Lexer &Lex;
  SourceManager &SourceMgr;
  tooling::Replacements Replaces;
  std::vector<CharSourceRange> Ranges;
  std::vector<UnwrappedLine> UnwrappedLines;
  bool StructuralError;
};

tooling::Replacements reformat(const FormatStyle &Style, Lexer &Lex,
                               SourceManager &SourceMgr,
                               std::vector<CharSourceRange> Ranges) {
  Formatter formatter(Style, Lex, SourceMgr, Ranges);
  return formatter.format();
}

}  // namespace format
}  // namespace clang
