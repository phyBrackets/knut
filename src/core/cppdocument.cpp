#include "cppdocument.h"
#include "cppdocument_p.h"

#include "functionsymbol.h"
#include "logger.h"
#include "project.h"
#include "settings.h"

#include <QFileInfo>
#include <QHash>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QVariantMap>

#include <kdalgorithms.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace Core {

/*!
 * \qmltype CppDocument
 * \brief Document object for a C++ file (source or header)
 * \inqmlmodule Script
 * \ingroup CppDocument/@first
 * \since 1.0
 * \inherits LspDocument
 */

/*!
 * \qmlproperty bool CppDocument::isHeader
 * Return true if the current document is a header.
 */

CppDocument::CppDocument(QObject *parent)
    : LspDocument(Type::Cpp, parent)
{
}

CppDocument::~CppDocument() = default;

static bool isHeaderSuffix(const QString &suffix)
{
    // Good enough for now, headers starts with h or hpp
    return suffix.startsWith('h');
}

bool CppDocument::isHeader() const
{
    QFileInfo fi(fileName());
    return isHeaderSuffix(fi.suffix());
}

/*!
 * \qmlmethod CppDocument::commentSelection()
 * Comments the selected lines (or current line if there's no selection) in current document.
 *
 * - If there's no selection, current line is commented using `//`.
 * - If there's a valid selection and the start and end position of the selection are before any text of the lines,
 *   all of the selected lines are commented using `//`.
 * - If there's a valid selection and the start and/or end position of the selection are between any text of the
 *   lines, all of the selected lines are commented using multi-line comment.
 * - If selection or position is invalid or out of range, or the position is on an empty line, the document remains
 *   unchanged.
 */
void CppDocument::commentSelection()
{
    LOG("CppDocument::commentSelection");

    QTextCursor cursor = textEdit()->textCursor();
    cursor.beginEditBlock();

    int cursorPos = cursor.position();
    int selectionOffset = 0;

    if (hasSelection()) {
        int selectionStartPos = cursor.selectionStart();
        int selectionEndPos = cursor.selectionEnd();

        // Preparing to check if the start and end positions of the selection are before any text of the lines
        cursor.setPosition(selectionStartPos);
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        const QString str1 = cursor.selectedText();
        cursor.setPosition(selectionEndPos);
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        const QString str2 = cursor.selectedText();

        if (str1.trimmed().isEmpty() && str2.trimmed().isEmpty()) {
            // Comment all lines in the selected region with "//"
            cursor.setPosition(selectionStartPos);
            cursor.movePosition(QTextCursor::StartOfLine);
            selectionStartPos = cursor.position();

            cursor.setPosition(selectionEndPos);
            // If the end of selection is at the beginning of the line, don't comment out the line the cursor is in.
            if (str2.isEmpty())
                cursor.movePosition(QTextCursor::Left);
            cursor.movePosition(QTextCursor::StartOfLine);

            do {
                cursor.insertText("//");
                selectionOffset += 2;
                cursor.movePosition(QTextCursor::Up);
                cursor.movePosition(QTextCursor::StartOfLine);
            } while (cursor.position() >= selectionStartPos);
        } else {
            // Comment the selected region using "/*" and "*/"
            cursor.setPosition(selectionEndPos);
            cursor.insertText("*/");
            selectionOffset += 2;
            cursor.setPosition(selectionStartPos);
            cursor.insertText("/*");
            selectionOffset += 2;
        }

        // Set the selection after commenting
        if (cursorPos == selectionEndPos) {
            cursor.setPosition(selectionStartPos);
            cursor.setPosition(selectionEndPos + selectionOffset, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(selectionEndPos + selectionOffset);
            cursor.setPosition(selectionStartPos, QTextCursor::KeepAnchor);
        }
    } else {
        cursor.select(QTextCursor::LineUnderCursor);
        // If the line is not empty, then comment it using "//"
        if (!cursor.selectedText().isEmpty()) {
            cursor.movePosition(QTextCursor::StartOfLine);
            cursor.insertText("//");
            selectionOffset += 2;
        }

        // Set the position after commenting
        cursor.setPosition(cursorPos + selectionOffset);
    }

    cursor.endEditBlock();
    textEdit()->setTextCursor(cursor);
}

static QStringList matchingSuffixes(bool header)
{
    static const auto mimeTypes =
        Settings::instance()->value<std::map<std::string, Document::Type>>(Settings::MimeTypes);

    QStringList suffixes;
    for (const auto &it : mimeTypes) {
        if (it.second == Document::Type::Cpp) {
            const QString suffix = QString::fromStdString(it.first);
            if ((header && !isHeaderSuffix(suffix)) || (!header && isHeaderSuffix(suffix)))
                suffixes.push_back(suffix);
        }
    }
    return suffixes;
}

static QStringList candidateFileNames(const QString &baseName, const QStringList &suffixes)
{
    QStringList result;
    result.reserve(suffixes.size());
    for (const auto &suffix : suffixes)
        result.push_back(baseName + '.' + suffix);
    return result;
}

static int commonFilePathLength(const QString &s1, const QString &s2)
{
    int length = qMin(s1.length(), s2.length());
    for (int i = 0; i < length; ++i) {
        if (s1[i].toLower() != s2[i].toLower())
            return i;
    }
    return length;
}

/*!
 * \qmlmethod string CppDocument::correspondingHeaderSource()
 * Returns the corresponding source or header file path.
 */
QString CppDocument::correspondingHeaderSource() const
{
    LOG("CppDocument::correspondingHeaderSource");
    static QHash<QString, QString> cache;

    QString cacheData = cache.value(fileName());
    if (!cacheData.isEmpty())
        return cacheData;

    const bool header = isHeader();
    const QStringList suffixes = matchingSuffixes(header);

    QFileInfo fi(fileName());
    QStringList candidates = candidateFileNames(fi.completeBaseName(), suffixes);

    // Search in the current directory
    for (const auto &candidate : candidates) {
        const QString testFileName = fi.absolutePath() + '/' + candidate;
        if (QFile::exists(testFileName)) {
            cache[fileName()] = testFileName;
            cache[testFileName] = fileName();
            spdlog::debug("CppDocument::correspondingHeaderSource {} => {}", fileName().toStdString(),
                          testFileName.toStdString());
            LOG_RETURN("path", testFileName);
        }
    }

    // Search in the whole project, and find the possible files
    QStringList fullPathNames = Project::instance()->allFilesWithExtensions(suffixes, Project::FullPath);
    auto notCandidate = [&candidates](const QString &path) {
        auto notInPath = [&path](const QString &candidate) {
            return !path.endsWith(candidate, Qt::CaseInsensitive);
        };
        return kdalgorithms::all_of(candidates, notInPath);
    };
    kdalgorithms::erase_if(fullPathNames, notCandidate);

    // Find the file having the most common path with fileName
    QString bestFileName;
    int compareValue = 0;
    for (const auto &path : fullPathNames) {
        int value = commonFilePathLength(path, fileName());
        if (value > compareValue) {
            compareValue = value;
            bestFileName = path;
        }
    }

    if (!bestFileName.isEmpty()) {
        cache[fileName()] = bestFileName;
        cache[bestFileName] = fileName();
        spdlog::debug("CppDocument::correspondingHeaderSource {} => {}", fileName().toStdString(),
                      bestFileName.toStdString());
        LOG_RETURN("path", bestFileName);
    }

    spdlog::warn("CppDocument::correspondingHeaderSource {} - not found ", fileName().toStdString());
    return {};
}

/*!
 * \qmlmethod CppDocument CppDocument::openHeaderSource()
 * Opens the corresponding source or header files, the current document is the new file.
 * If no files have been found, it's a no-op.
 */
CppDocument *CppDocument::openHeaderSource()
{
    LOG("CppDocument::openHeaderSource");
    const QString fileName = correspondingHeaderSource();
    if (!fileName.isEmpty())
        LOG_RETURN("document", qobject_cast<CppDocument *>(Project::instance()->open(fileName)));
    return nullptr;
}

/*!
 * \qmlmethod array<QueryMatch> CppDocument::queryMethodDefinition(string scope, string methodName)
 *
 * Returns the list of methods definitions matching the given name and scope.
 * `scope` may be either a class name, a namespace or empty.
 *
 * Every QueryMatch returned by this function will have the following captures available:
 *
 * - `scope` - The scope of the method (if any is provided)
 * - `name` - The name of the function
 * - `definition` - The entire method definition
 * - `parameter-list` - The list of parameters
 * - `parameters` - One capture per parameter, containing the type and name of the parameter, excluding comments!
 * - `body` - The body of the method (including curly-braces)
 */
QVector<QueryMatch> CppDocument::queryMethodDefinition(const QString &scope, const QString &functionName)
{
    // Clang-format gets confused by the raw strings
    // clang-format off
    auto identifier = QString(R"EOF(
            (identifier) @name (#eq? @name "%1")
        )EOF").arg(functionName);

    if (!scope.isEmpty()) {
        identifier = QString(R"EOF(
            (qualified_identifier
                scope: (_) @scope (#eq? @scope "%1")
                %2
            )
        )EOF").arg(scope, identifier);
    }

    const auto queryString = QString(R"EOF(
        (function_definition
            type: (_)? @returnType
            declarator: (function_declarator
                declarator: %1
                parameters: (parameter_list
                    (parameter_declaration)* @parameters
                ) @parameter-list)
            body: (compound_statement) @body
        ) @definition
    )EOF").arg(identifier);
    //clang-format on

    return query(queryString);
}

/*!
 * \qmlmethod CppDocument::insertCodeInMethod(string methodName, string code, Position insertAt)
 *
 * Provides a fast way to add some code in an existing method definition. Does nothing if the method does not exist in
 * the current document.
 *
 * This method will find a method in the current file with name matching with `methodName`. If the method exists in the
 * current document, then it will insert the supplied `code` either at the beginning of the method, or at the end of the
 * method, depending on the `insertAt` argument.
 */
bool CppDocument::insertCodeInMethod(const QString &methodName, QString code, Position insertAt)
{
    LOG("CppDocument::insertCodeInMethod", methodName, code, insertAt);

    auto symbol = findSymbol(methodName);
    if (!symbol) {
        spdlog::warn("CppDocument::insertCodeInMethod: No symbol found for {}.", methodName.toStdString());
        return false;
    }

    if (!symbol->isFunction()) {
        spdlog::warn("CppDocument::insertCodeInMethod: {} is not a function or a method.",
                     symbol->name().toStdString());
        return false;
    }

    QTextCursor cursor = textEdit()->textCursor();
    cursor.setPosition(symbol->range().end);
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor);
    if (cursor.selectedText() != "}") {
        spdlog::warn("CppDocument::insertCodeInMethod: {} is not a function definition.", symbol->name().toStdString());
        return false;
    }

    cursor.beginEditBlock();
    // Goto the end and move back one character
    cursor.setPosition(symbol->range().end);
    cursor.movePosition(QTextCursor::PreviousCharacter);

    const QString strTab = tab();
    if (insertAt == StartOfMethod) {
        // Goto the start of the block
        textEdit()->setTextCursor(cursor);
        cursor.setPosition(gotoBlockStart());
        // Move forward one character
        cursor.movePosition(QTextCursor::NextCharacter);
        // Insert a new line
        cursor.insertText("\n");
    }

    // Insert an indent before the first line
    cursor.insertText(strTab);
    // Insert an indent before every line in the code
    code.replace("\n", ("\n" + strTab));

    if (insertAt == EndOfMethod)
        if (!code.endsWith("\n" + strTab))
            code.append("\n");

    // If there's an extra tab at the end, chop it
    if (code.endsWith(strTab))
        code.chop(strTab.length());

    cursor.insertText(code);
    cursor.endEditBlock();

    textEdit()->setTextCursor(cursor);

    return true;
}

/*!
 * \qmlmethod CppDocument::insertForwardDeclaration(string fwddecl)
 * Inserts the forward declaration `fwddecl` into the current file.
 * The method will check if the file is a header file, and also that the forward declaration starts with 'class ' or
 * 'struct '. Fully qualified the forward declaration to add namespaces: `class Foo::Bar::FooBar` will result in:
 *
 * ```cpp
 * namespace Foo {
 * namespace Bar {
 * class FooBar
 * }
 * }
 * ```
 */
bool CppDocument::insertForwardDeclaration(const QString &fwddecl)
{
    LOG("CppDocument::insertForwardDeclaration", LOG_ARG("text", fwddecl));
    if (!isHeader()) {
        spdlog::warn("CppDocument::insertForwardDeclaration: {} - is not a header file. ", fileName().toStdString());
        return false;
    }

    int spacePos = fwddecl.indexOf(' ');
    auto classOrStruct = QStringView(fwddecl).left(spacePos);
    if (fwddecl.isEmpty() || (classOrStruct != QStringLiteral("class") && classOrStruct != QStringLiteral("struct"))) {
        spdlog::warn("CppDocument::insertForwardDeclaration: {} - should start with 'class ' or 'struct '. ",
                     fwddecl.toStdString());
        return false;
    }

    auto qualifierList = QStringView(fwddecl).mid(spacePos + 1).split(QStringLiteral("::"));
    std::ranges::reverse(qualifierList);

    // Get the un-qualified declaration
    QString result = QString("%1 %2;").arg(classOrStruct).arg(qualifierList.first());
    qualifierList.pop_front();

    // Check if the declaration already exists
    QTextDocument *doc = textEdit()->document();
    QTextCursor cursor(doc);
    cursor = doc->find(result, cursor, QTextDocument::FindWholeWords);
    if (!cursor.isNull()) {
        spdlog::warn("CppDocument::insertForwardDeclaration: '{}' - already exists in file.", fwddecl.toStdString());
        return false;
    }

    for (const auto &qualifier : std::as_const(qualifierList))
        result = QString("namespace %1 {\n%2\n}").arg(qualifier, result);

    cursor = QTextCursor(doc);
    cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
    cursor = doc->find(QRegularExpression(QStringLiteral(R"(^#include\s*)")), cursor, QTextDocument::FindBackward);
    if (!cursor.isNull()) {
        cursor.beginEditBlock();
        cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::MoveAnchor);
        cursor.insertText("\n\n" + result);
        cursor.endEditBlock();
        return true;
    }

    return false;
}

/*!
 * \qmlmethod map<string, string> CppDocument::mfcExtractDDX()
 * Extracts the DDX information from a MFC class.
 *
 * The DDX information gives the mapping between the ID and the member variables in the class.
 */
QVariantMap CppDocument::mfcExtractDDX(const QString &className)
{
    LOG("CppDocument::mfcExtractDDX", LOG_ARG("text", className));

    QVariantMap map;

    // TODO: Use semantic information coming from LSP instead of regexp to find the method

    const QString source = text();
    static const QRegularExpression searchFunctionExpression(
        QString(R"*(void\s*%1\s*::DoDataExchange\s*\()*").arg(className), QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = searchFunctionExpression.match(source);

    if (match.hasMatch()) {
        const int capturedStart = match.capturedStart(0);
        const int capturedEnd = match.capturedEnd(0);
        int bracketCount = 0;
        int positionEnd = -1;
        for (int i = capturedEnd; i < source.length(); ++i) {
            if (source.at(i) == QLatin1Char('{')) {
                bracketCount++;
            } else if (source.at(i) == QLatin1Char('}')) {
                bracketCount--;
                if (bracketCount == 0) {
                    positionEnd = i;
                    break;
                }
            }
        }

        if (positionEnd == -1)
            return {};

        const QString ddxText = source.mid(capturedStart, (positionEnd - capturedStart + 1));
        static const QRegularExpression doDataExchangeExpression(R"*(DDX_.*\(.*,\s*(.*)\s*,\s*(.*)\))*");
        QRegularExpressionMatchIterator userIteratorWidgetExpression = doDataExchangeExpression.globalMatch(ddxText);
        while (userIteratorWidgetExpression.hasNext()) {
            const QRegularExpressionMatch match = userIteratorWidgetExpression.next();
            map.insert(match.captured(1), match.captured(2));
        }
    }
    return map;
}

/*!
 * \qmlmethod CppDocument::mfcExtractMessageMap(className = "")
 * \since 1.1
 *
 * Extracts information contained in the MFC MESSAGE_MAP.
 * The `className` parameter can be used to ensure the result matches to a specific class.
 * Returns a `MessageMap` object.
 */
MessageMap CppDocument::mfcExtractMessageMap(const QString &className /* = ""*/)
{
    auto checkClassName = className.isEmpty() ? "" : QString("(#eq? @class \"%1\")").arg(className);

    // clang-format off
    auto queryString = QString(R"EOF(
        (translation_unit
        ; Assumption: the MESSAGE_MAP is always top-level

            ; Group to make sure the nodes are actually siblings
            (

                ; Search for BEGIN_MESSAGE_MAP
                (expression_statement
                    (call_expression
                        function: (identifier) @begin_ident
                        (#eq? @begin_ident "BEGIN_MESSAGE_MAP")
                        arguments: (argument_list
                             (identifier) @class
                             %1 ; If a class name is given, check if the captured class name matches
                             (identifier) @superclass)) @begin)

                ; Followed by one or more entries
                [
                (expression_statement
                    (call_expression
                        function: (identifier) @message-name
                        arguments: (argument_list
                            ((_) @parameter ("," (_) @parameter)*)?)
                ))@message
                (_)
                ]*

                ; Ending with END_MESSAGE_MAP
                (expression_statement
                    (call_expression
                        function: (identifier) @end_ident
                        (#eq? @end_ident "END_MESSAGE_MAP")) @end)
            )

        )
    )EOF").arg(checkClassName);
    // clang-format on

    auto result = query(queryString);
    if (result.isEmpty()) {
        spdlog::warn("CppDocument::findMessageMap: No message map found in `{}`", fileName().toStdString());
        return {};
    }

    const auto &match = result.first();

    return MessageMap(match);
}

/*!
 * \qmlmethod CppDocument::mfcReplaceAfxMsgDeclaration(string afxMsgName, string newDeclaration)
 * \since 1.1
 *
 * Replaces the declaration of an afx_msg with `afxMsgName` with a new declaration.
 */
bool CppDocument::mfcReplaceAfxMsgDeclaration(const QString &afxMsgName, const QString &newDeclaration)
{
    // clang-format off
    auto queryString = QString(R"EOF(
        (field_declaration
            type: (_) @type (#eq? @type "afx_msg")
            (function_declarator
                declarator: (field_identifier) @name (#eq? @name "%1")
            )) @function
    )EOF").arg(afxMsgName);
    // clang-format on

    auto matches = query(queryString);

    if (matches.isEmpty()) {
        spdlog::warn("CppDocument::mfcReplaceAfxMsgDeclaration: No afx_msg named `{}` found in `{}`",
                     afxMsgName.toStdString(), fileName().toStdString());
        return false;
    }

    if (matches.size() > 1) {
        spdlog::warn("CppDocument::mfcReplaceAfxMsgDeclaration: Multiple afx_msg named `{}` found in `{}`!",
                     afxMsgName.toStdString(), fileName().toStdString());
    }

    for (const auto &match : matches) {
        match.get("function").replace(newDeclaration);
    }

    return true;
}

/*!
 * \qmlmethod int CppDocument::gotoBlockStart(int count)
 * Moves the cursor to the start of the block it's in, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Does it `count` times.
 */
int CppDocument::gotoBlockStart(int count)
{
    LOG_AND_MERGE("CppDocument::gotoBlockStart", count);

    QTextCursor cursor = textEdit()->textCursor();
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::PreviousCharacter));
        --count;
    }
    textEdit()->setTextCursor(cursor);
    return cursor.position();
}

/*!
 * \qmlmethod int CppDocument::gotoBlockEnd(int count)
 * Moves the cursor to the end of the block it's in, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Does it `count` times.
 */
int CppDocument::gotoBlockEnd(int count)
{
    LOG_AND_MERGE("CppDocument::gotoBlockEnd", count);

    QTextCursor cursor = textEdit()->textCursor();
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::NextCharacter));
        --count;
    }
    textEdit()->setTextCursor(cursor);
    return cursor.position();
}

/*!
 * \qmlmethod int CppDocument::selectBlockStart()
 * Selects the text from current cursor position to the start of the block, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Does it `count` times.
 */
int CppDocument::selectBlockStart(int count)
{
    LOG_AND_MERGE("CppDocument::selectBlockStart", count);

    QTextCursor cursor = textEdit()->textCursor();
    const int selectionStart = std::max(cursor.selectionStart(), cursor.selectionEnd());
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::PreviousCharacter));
        --count;
    }
    const int blockStartPos = cursor.position();

    cursor.setPosition(selectionStart, QTextCursor::MoveAnchor);
    cursor.setPosition(blockStartPos, QTextCursor::KeepAnchor);

    textEdit()->setTextCursor(cursor);
    return blockStartPos;
}

/*!
 * \qmlmethod int CppDocument::selectBlockEnd()
 * Selects the text from current cursor position to the end of the block, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Does it `count` times.
 */
int CppDocument::selectBlockEnd(int count)
{
    LOG_AND_MERGE("CppDocument::selectBlockEnd", count);

    QTextCursor cursor = textEdit()->textCursor();
    const int selectionStart = std::min(cursor.selectionStart(), cursor.selectionEnd());
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::NextCharacter));
        --count;
    }
    const int blockEndPos = cursor.position();

    cursor.setPosition(selectionStart, QTextCursor::MoveAnchor);
    cursor.setPosition(blockEndPos, QTextCursor::KeepAnchor);

    textEdit()->setTextCursor(cursor);
    return blockEndPos;
}

/*!
 * \qmlmethod int CppDocument::selectBlockUp()
 * \since 1.1
 * Selects the text of the block the cursor is in, and returns the new cursor position.
 * A block is definied by {} or () or [].
 * Does it `count` times.
 */
int CppDocument::selectBlockUp(int count)
{
    LOG_AND_MERGE("CppDocument::selectBlockUp", count);

    QTextCursor cursor = textEdit()->textCursor();
    while (count != 0) {
        cursor.setPosition(moveBlock(cursor.position(), QTextCursor::NextCharacter));
        --count;
    }
    const int blockEndPos = cursor.position();
    const int blockStartPos = moveBlock(cursor.position(), QTextCursor::PreviousCharacter);
    cursor.setPosition(blockStartPos, QTextCursor::MoveAnchor);
    cursor.setPosition(blockEndPos, QTextCursor::KeepAnchor);

    textEdit()->setTextCursor(cursor);
    return blockEndPos;
}

/**
 * \brief Internal method to move to the start or end of a block
 * \param startPos current cursor position
 * \param direction the iteration
 * \return position of the start or end of the block
 */
int CppDocument::moveBlock(int startPos, QTextCursor::MoveOperation direction)
{
    Q_ASSERT(direction == QTextCursor::NextCharacter || direction == QTextCursor::PreviousCharacter);

    QTextDocument *doc = textEdit()->document();
    Q_ASSERT(doc);

    const int inc = direction == QTextCursor::NextCharacter ? 1 : -1;
    const int lastPos = direction == QTextCursor::NextCharacter ? textEdit()->document()->characterCount() - 1 : 0;
    if (startPos == lastPos)
        return startPos;
    int pos = startPos + inc;
    QChar currentChar = doc->characterAt(pos);

    // Set the characters delimiter that increment or decrement the counter when iterating
    auto incCounterChar =
        direction == QTextCursor::NextCharacter ? QVector<QChar> {'(', '{', '['} : QVector<QChar> {')', '}', ']'};
    auto decCounterChar =
        direction == QTextCursor::PreviousCharacter ? QVector<QChar> {'(', '{', '['} : QVector<QChar> {')', '}', ']'};

    // If the character next is a special one, go inside the block
    if (incCounterChar.contains(currentChar))
        pos += inc;

    // Iterate to find the other side of the block
    int counter = 0;
    pos += inc;

    auto hitLastChar = [direction, lastPos](int pos) {
        return direction == QTextCursor::NextCharacter ? pos >= lastPos : pos <= lastPos;
    };

    while (!hitLastChar(pos)) {
        currentChar = doc->characterAt(pos);

        if (incCounterChar.contains(currentChar)) {
            counter++;

        } else if (decCounterChar.contains(currentChar)) {
            counter--;

            // When counter is negative, we have found the other side of the block
            if (counter < 0)
                return pos + std::max(inc, 0);
        }
        pos += inc;
    }
    return startPos;
}

/*!
 * \qmlmethod CppDocument::toggleSection()
 * Comments out a section of the code using `#ifdef` / `#endif`. The variable used is defined by the settings.
 * ```json
 * "toggle_section": {
 *     "tag": "KDAB_TEMPORARILY_REMOVED",
 *     "debug": "qDebug(\"%1 is commented out\")"
 *     "return_values": {
 *         "BOOL": "false"
 *     }
 * }
 * ```
 * `debug` is the debug line to show, if empty it won't show anything. `return_values` gives a mapping for the value
 * returned by the function. In this example, if the returned type is `BOOL`, it will return `false`. If text is
 * selected, it comment out the lines of the selected text. Otherwise, it will comment the function the cursor is in. In
 * the latter case, if the function is already commented, it will remove the commented section.
 */
void CppDocument::toggleSection()
{
    LOG("CppDocument::toggleSection");

    auto sectionSettings = Settings::instance()->value<ToggleSectionSettings>(Settings::ToggleSection);
    const QString endifString = QStringLiteral("#endif // ") + sectionSettings.tag;
    const QString ifdefString = QStringLiteral("#ifdef ") + sectionSettings.tag;
    const QString elseString = QStringLiteral("#else // ") + sectionSettings.tag;
    const QString newLine = QStringLiteral("\n");

    QTextCursor cursor = textEdit()->textCursor();
    if (cursor.hasSelection()) {
        // If there's a selection, just add #ifdef/#endif
        cursor.beginEditBlock();
        auto [min, max] = std::minmax(cursor.selectionStart(), cursor.selectionEnd());
        int line, col;
        convertPosition(max, &line, &col);
        // Add #endif / #ifdef, starts to the end or min/max won't be right
        cursor.setPosition(position(QTextCursor::EndOfLine, col == 1 ? max - 1 : max));
        cursor.insertText(newLine + endifString);
        cursor.setPosition(position(QTextCursor::StartOfLine, min));
        cursor.insertText(ifdefString + newLine);
        // Move after the #endif
        cursor.endEditBlock();
        textEdit()->setTextCursor(cursor);
        gotoLine(line + 3);

    } else {
        // Check that we are in a function
        auto isFunction = [](const Symbol &symbol) {
            return symbol.isFunction();
        };
        auto symbol = currentSymbol(isFunction);
        if (!symbol)
            return;

        auto cursorPos = cursor.position();

        cursor.beginEditBlock();
        // Start from the end
        cursor.setPosition(symbol->range().end);
        cursor.movePosition(QTextCursor::StartOfLine);
        cursor.movePosition(QTextCursor::Up, QTextCursor::KeepAnchor);

        if (cursor.selectedText().startsWith(endifString)) {
            // The function is already commented out, remove the comments
            int start = textEdit()->document()->find(elseString, cursor, QTextDocument::FindBackward).selectionStart();
            if (start > symbol->range().start)
                cursor.setPosition(start, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            cursor.setPosition(moveBlock(cursor.position(), QTextCursor::PreviousCharacter));
            cursor.movePosition(QTextCursor::Down);
            cursor.movePosition(QTextCursor::StartOfLine);
            cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            cursorPos -= ifdefString.length() + 1;
        } else {
            // Comment out the function with #if/#def, make sure to return something if needed
            cursor.setPosition(symbol->range().end);
            cursor.movePosition(QTextCursor::PreviousCharacter);

            QString text = elseString + newLine;
            if (!sectionSettings.debug.isEmpty())
                text += tab() + sectionSettings.debug.arg(symbol->name()) + ";\n";
            const QString returnType = symbol->toFunction()->returnType();
            auto it = sectionSettings.return_values.find(returnType.toStdString());
            if (it != sectionSettings.return_values.end())
                text += tab() + QString("return %1;\n").arg(QString::fromStdString(it->second));
            else if (returnType.isEmpty() || returnType == "void")
                text += tab() + "return;\n";
            else if (returnType.endsWith('*'))
                text += tab() + "return nullptr;\n";
            else
                text += tab() + "return {};\n";
            text += endifString + newLine;
            cursor.insertText(text);

            cursor.setPosition(moveBlock(cursor.position(), QTextCursor::PreviousCharacter));
            cursor.movePosition(QTextCursor::NextCharacter);
            cursor.insertText(newLine + ifdefString);
            cursorPos += ifdefString.length() + 1;
        }
        cursor.endEditBlock();
        textEdit()->setTextCursor(cursor);
        setPosition(cursorPos);
    }
}

/*!
 * \qmlmethod CppDocument::insertInclude(string include, bool newGroup = false)
 * Inserts a new include line in the file. If the include is already in, do nothing (and returns true).
 *
 * The `include` string should be either `<foo.h>` or `"foo.h"`, it will returns false otherwise.
 * The method will try to find the best group of includes to insert into, a group of includes being consecutive includes
 * in the file.
 *
 * If `newGroup` is true, it will insert the include at the end, with a new line separating the other includes.
 */
bool CppDocument::insertInclude(const QString &include, bool newGroup)
{
    LOG("CppDocument::insertInclude", LOG_ARG("text", include), newGroup);

    IncludeHelper includeHelper(this);
    auto includePos = includeHelper.includePositionForInsertion(include, newGroup);
    if (!includePos) {
        spdlog::error(R"(CppDocument::insertInclude - the include '{}' is malformed, should be '<foo.h>' or '"foo.h"')",
                      include.toStdString());
        return false;
    }

    if (includePos->alreadyExists()) {
        spdlog::info("CppDocument::insertInclude - the include '{}' is already included.", include.toStdString());
        return true;
    }

    const QString text = (includePos->newGroup ? "\n#include " : "#include ") + include + '\n';
    insertAtLine(text, includePos->line);
    return true;
}

RangeMark CppDocument::findClassBody(const QString &className)
{
    LOG("CppDocument::findClassBody", LOG_ARG("className", className));

    // clang-format off
    const auto classDefinitionQuery = QString(R"EOF(
        (class_specifier
            name:(_) @className (#eq? @className "%1")
            body: (_) @classBody
        )
    )EOF").arg(className);
    // clang-format on

    auto result = query(classDefinitionQuery);

    if (!result.isEmpty()) {

        const auto &match = result.first();
        auto classMatch = match.get("classBody");

        return createRangeMark(classMatch.start(), classMatch.end());
    }
    return {};
}

CppDocument::MemberOrMethodAdditionResult
CppDocument::addMemberOrMethod(const QString &memberInfo, const QString &className, AccessSpecifier specifier)
{

    QString memberText = memberInfo + ";";
    // clang-format off
    const auto queryString = QString(R"EOF(
        (field_declaration_list
            (access_specifier "%1") @access
            . [(declaration) (comment) (field_declaration)]* @field
        )
    )EOF").arg(accessSpecifierMap.value(specifier));
    // clang-format on

    const auto range = findClassBody(className);
    if (!range.isValid()) {
        return MemberOrMethodAdditionResult::ClassNotFound;
    }

    auto result = queryInRange(range, queryString);
    if (!result.isEmpty()) {
        const auto &match = result.last();
        const auto fields = match.getAll("field");
        const auto pos = fields.last();
        const auto indent = indentationAtPosition(pos.end());
        insertAtPosition("\n" + indent + memberText, pos.end());
    } else {
        const bool check = addSpecifierSection(memberText, className, specifier);
        if (!check) {
            return MemberOrMethodAdditionResult::ClassNotFound;
        }
    }

    return MemberOrMethodAdditionResult::Success;
}

/*!
 * \qmlmethod CppDocument::addMember(string member, string className, AccessSpecifier)
 * \since 1.1
 * Adds a new member in a specific class under the specefic access specifier.
 *
 * If the class does not exist, log error can't find the class, but if the
 * specifier is valid but does not exist in the class, we will add that specifier in the end of the
 * class and add the member under it.
 * The specifier can take these values:
 *
 * - `CppDocument.Public`
 * - `CppDocument.Protected`
 * - `CppDocument.Private`
 */
bool CppDocument::addMember(const QString &member, const QString &className, AccessSpecifier specifier)
{
    LOG("CppDocument::addMember", member, className, specifier);

    auto result = addMemberOrMethod(member, className, specifier);
    if (result == MemberOrMethodAdditionResult::ClassNotFound) {
        spdlog::error(R"(CppDocument::addMember- Can't find class '{}')", className.toStdString());
    }

    return true;
}

/*!
 * \qmlmethod CppDocument::addMethodDeclaration(string member, string className, AccessSpecifier)
 * \since 1.1
 * Declares a new method in a specific class under the specefic access specifier.
 *
 * If the class does not exist, log error can't find the class, but if the
 * specifier is valid but does not exist in the class, we will add that specifier in the end of the
 * class and declare the method under it.
 * The specifier can take these values:
 *
 * - `CppDocument.Public`
 * - `CppDocument.Protected`
 * - `CppDocument.Private`
 */
bool CppDocument::addMethodDeclaration(const QString &method, const QString &className, AccessSpecifier specifier)
{
    LOG("CppDocument::addMethodDeclaration", method, className, specifier);

    auto result = addMemberOrMethod(method, className, specifier);
    if (result == MemberOrMethodAdditionResult::ClassNotFound) {
        spdlog::error(R"(CppDocument::addMethodDeclaration - Can't find class '{}')", className.toStdString());
    }

    return true;
}

/*!
 * \qmlmethod CppDocument::addMethodDefintion(string declaration, string className)
 * \qmlmethod CppDocument::addMethodDefintion(string declaration, string className, string body)
 * \since 1.1
 *
 * Adds a new method definition for the method declared by the given `declaration` for
 * class `className` in the current file.
 * The provided `body` should not include the curly braces.
 *
 * If no body is provided, it will default to an empty body.
 */
bool CppDocument::addMethodDefinition(const QString &declaration, const QString &className,
                                      const QString &body /*= ""*/)
{
    LOG("CppDocument::addMethodDefinition", declaration, className);

    QString definition = declaration;

    // Remove declaration specific modifiers to make the parameter compatible with addMethodDeclaration
    QStringList modifiers = {"override", "final", "virtual", "static", "Q_INVOKABLE", "Q_SLOT", "Q_SIGNAL"};
    for (const auto &modifier : modifiers) {
        definition.remove(modifier);
    }
    definition = definition.simplified();

    // Extract the return type and method name
    int openParenIdx = definition.indexOf('(');
    int spaceIdx = definition.lastIndexOf(' ', openParenIdx);

    QString returnType = definition.left(spaceIdx);
    QString methodName = definition.mid(spaceIdx + 1, openParenIdx - spaceIdx - 1);

    // Construct the method definition
    QString methodDef = QString("%1 %2::%3").arg(returnType, className, methodName);
    QString fullBody = body.isEmpty() ? " {}" : QString(" {\n%1\n}").arg(body);
    methodDef += definition.mid(openParenIdx) + fullBody;

    QString indent = "\n\n";

    auto lastBracePos = textEdit()->toPlainText().lastIndexOf('}');

    QTextCursor cursor = textEdit()->textCursor();
    cursor.beginEditBlock();

    cursor.setPosition(lastBracePos + 1);
    cursor.movePosition(QTextCursor::EndOfBlock);

    // Add the method definition
    cursor.insertText(indent + methodDef);
    auto methodStartPos = textEdit()->toPlainText().lastIndexOf('{');
    cursor.setPosition(methodStartPos + 1); // move to position after opening brace
    cursor.endEditBlock();

    textEdit()->setTextCursor(cursor);
    return true;
}

/*!
 * \qmlmethod CppDocument::addMethod(string declaration, string className, AccessSpecifier, string body)
 * \qmlmethod CppDocument::addMethod(string declaration, string className, AccessSpecifier)
 * \since 1.1
 *
 * Declares and defines a new method.
 * This method can be called on either the header or source file.
 * It will find the corresponding header/source file and add the declaration
 * to the header and the definition to the source.
 *
 * \sa addMethodDeclaration
 * \sa addMethodDefinition
 */
bool CppDocument::addMethod(const QString &declaration, const QString &className, AccessSpecifier specifier,
                            const QString &body /*= ""*/)
{
    auto header = this;
    auto source = openHeaderSource();

    if (!isHeader()) {
        std::swap(header, source);
    }

    auto result = true;

    if (header) {
        result &= header->addMethodDeclaration(declaration, className, specifier);
    } else {
        spdlog::error("CppDocument::addMethod - Can't find header file for '{}'", className.toStdString());
    }

    if (source) {
        result &= source->addMethodDefinition(declaration, className, body);
    } else {
        spdlog::error("CppDocument::addMethod - Can't find source file for '{}'", className.toStdString());
    }

    return result;
}

bool CppDocument::addSpecifierSection(const QString &memberText, const QString &className, AccessSpecifier specifier)
{

    // clang-format off
    const auto queryString = QString(R"EOF(
        (field_declaration_list
            (_)@pos
        )
    )EOF");
    // clang-format on

    auto range = findClassBody(className);
    auto result = queryInRange(range, queryString);

    if (!result.isEmpty()) {
        const auto &match = result.last();
        const auto pos = match.get("pos");
        const auto indent = indentationAtPosition(pos.end());

        const QString newSpecifier = QString("\n\n%1:").arg(accessSpecifierMap.value(specifier));

        insertAtPosition(newSpecifier + "\n" + indent + memberText, pos.end());
    } else {
        // The class specifier is invalid
        return false;
    }
    return true;
}

/*!
 * \qmlmethod CppDocument::removeInclude(string include)
 * Remove `include` from the file. If the include is not in the file, do nothing (and returns true).
 *
 * The `include` string should be either `<foo.h>` or `"foo.h"`, it will returns false otherwise.
 */
bool CppDocument::removeInclude(const QString &include)
{
    LOG("CppDocument::removeInclude", LOG_ARG("text", include));

    IncludeHelper includeHelper(this);
    auto line = includeHelper.includePositionForRemoval(include);
    if (!line) {
        spdlog::error(R"(CppDocument::removeInclude - the include '{}' is malformed, should be '<foo.h>' or '"foo.h"')",
                      include.toStdString());
        return false;
    }

    if (line.value() == -1) {
        spdlog::info("CppDocument::removeInclude - the include '{}' is not included.");
        return true;
    }

    deleteLine(line.value());
    return true;
}

/**
 * Delete the fully qualified method with the given signature.
 * If signature is empty, will delete all overloads.
 * The method will only be deleted within this document, no other documents will be modified.
 */
void CppDocument::deleteMethodLocal(const QString &methodName, const QString &signature)
{
    auto doesNotMatchMethod = [&methodName, &signature](const auto &symbol) {
        const auto isFunction = symbol->isFunction();

        if (signature.isEmpty())
            return !isFunction || symbol->name() != methodName;
        else
            return !isFunction || symbol->name() != methodName || symbol->description() != signature;
    };

    auto symbolList = symbols();
    kdalgorithms::erase_if(symbolList, doesNotMatchMethod);
    if (symbolList.empty())
        return;

    // Sort the symbols so that we remove them end-to-start
    // That way removing a function won't change the position of the other functions.
    // This assumes the ranges don't overlap.
    auto byRange = [](const auto &symbol1, const auto &symbol2) {
        return symbol1->range().start > symbol2->range().start;
    };
    std::ranges::sort(symbolList, byRange);

    for (const auto &symbol : symbolList) {
        spdlog::trace("CppDocument::deleteMethodLocal: Removing symbol '{}'", symbol->name().toStdString());

        deleteSymbol(*symbol);
    }
}

/*!
 * \qmlmethod void CppDocument::deleteMethod(string methodName, string signature)
 * \since 1.1
 *
 * Delete the method or function with the specified `methodName` and optional `signature`.
 * The method definition/declaration will be deleted from the current file,
 * as well as the corresponding header/source file.
 * References to the method will not be deleted.
 *
 * The `methodName` must be fully qualified, i.e. "<Namespaces>::<Class>::<Method>".
 *
 * The `signature` must be in the form: "<return type> (<first parameter type>, <second parameter type>, <...>)".
 * i.e. for a function with the following declaration:
 *
 * ``` cpp
 * void myFunction(const QString& a, int b);
 * ```
 *
 * The `signature` would be:
 *
 * ``` cpp
 * void (const QString&, int)
 * ```
 *
 * If an empty string is provided as the `signature`, all overloads of the function are deleted as well.
 */
void CppDocument::deleteMethod(const QString &methodName, const QString &signature)
{
    LOG("CppDocument::deleteMethod", methodName, signature);

    QString headerSourceName = correspondingHeaderSource();
    if (!headerSourceName.isEmpty()) {
        auto headerSource = qobject_cast<CppDocument *>(Project::instance()->get(headerSourceName));
        headerSource->deleteMethodLocal(methodName, signature);
    }
    deleteMethodLocal(methodName, signature);
}

/*!
 * \qmlmethod void CppDocument::deleteMethod(string methodName)
 * \since 1.1
 *
 * Deletes a method of the specified `methodName`, without matching a specific `signature`.
 * Therefore, all overloads of the function will be deleted.
 *
 * Also see: CppDocument::deleteMethod(string methodName, string signature)
 */
void CppDocument::deleteMethod(const QString &methodName)
{
    LOG("CppDocument::deleteMethod", LOG_ARG("text", methodName));

    deleteMethod(methodName, "" /*empty string means ignore signature*/);
}

/*!
 * \qmlmethod void CppDocument::deleteMethod()
 * \since 1.1
 *
 * Deletes the method/function at the current cursor position.
 * Overloads of the function will not be deleted!
 *
 * Also see: CppDocument::deleteMethod(const QString& methodName, const QString& signature)
 */
void CppDocument::deleteMethod()
{
    LOG("CppDocument::deleteMethod");

    auto isFunction = [](const auto &symbol) {
        return symbol.isFunction();
    };

    auto symbol = currentSymbol(isFunction);

    if (!symbol) {
        spdlog::error(
            "CppDocument::deleteMethod: Cursor is not currently within a function definition or declaration!");
    } else {
        deleteMethod(symbol->name(), symbol->description());
    }
}

} // namespace Core
