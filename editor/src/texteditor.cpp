/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *               2011 ~ 2018 Wang Yong
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Wang Yong <wangyong@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "texteditor.h"
#include "utils.h"
#include "window.h"

#include "Definition"
#include "SyntaxHighlighter"
#include "Theme"

#include <QTextDocumentFragment>
#include <DDesktopServices>
#include <QApplication>
#include <DSettingsGroup>
#include <DSettingsOption>
#include <DSettings>
#include <QClipboard>
#include <QFileInfo>
#include <QDebug>
#include <QPainter>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTextBlock>
#include <QMimeData>
#include <QTimer>

class LineNumberArea : public QWidget
{
public:
    LineNumberArea(TextEditor *editor)
        : QWidget(editor),
          editor(editor) {
    }

    void paintEvent(QPaintEvent *event) {
        editor->lineNumberAreaPaintEvent(event);
    }

    TextEditor *editor;
};

TextEditor::TextEditor(QPlainTextEdit *parent)
    : QPlainTextEdit(parent),
      m_highlighter(new KSyntaxHighlighting::SyntaxHighlighter(document()))
{
    viewport()->installEventFilter(this);

    // Don't draw frame around editor widget.
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);

    // Init widgets.
    lineNumberArea = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::updateRequest, this, &TextEditor::handleUpdateRequest);
    connect(this, &QPlainTextEdit::textChanged, this, &TextEditor::updateLineNumber, Qt::QueuedConnection);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &TextEditor::highlightCurrentLine, Qt::QueuedConnection);
    connect(document(), &QTextDocument::modificationChanged, this, &TextEditor::setModified);

    // Init menu.
    m_rightMenu = new QMenu();
    m_rightMenu->setStyle(QStyleFactory::create("dlight"));
    m_undoAction = new QAction(tr("Undo"), this);
    m_redoAction = new QAction(tr("Redo"), this);
    m_cutAction = new QAction(tr("Cut"), this);
    m_copyAction = new QAction(tr("Copy"), this);
    m_pasteAction = new QAction(tr("Paste"), this);
    m_deleteAction = new QAction(tr("Delete"), this);
    m_selectAllAction = new QAction(tr("Select All"), this);
    m_findAction = new QAction(tr("Find"), this);
    m_replaceAction = new QAction(tr("Replace"), this);
    m_jumpLineAction = new QAction(tr("Go to Line"), this);
    m_enableReadOnlyModeAction = new QAction(tr("Turn on Read-Only mode"), this);
    m_disableReadOnlyModeAction = new QAction(tr("Turn off Read-Only mode"), this);
    m_fullscreenAction = new QAction(tr("Fullscreen"), this);
    m_exitFullscreenAction = new QAction(tr("Exit fullscreen"), this);
    m_openInFileManagerAction = new QAction(tr("Open in file manager"), this);
    m_toggleCommentAction = new QAction(tr("Toggle comment"), this);

    connect(m_rightMenu, &QMenu::aboutToHide, this, &TextEditor::removeHighlightWordUnderCursor);
    connect(m_undoAction, &QAction::triggered, this, &TextEditor::undo);
    connect(m_redoAction, &QAction::triggered, this, &TextEditor::redo);
    connect(m_cutAction, &QAction::triggered, this, &TextEditor::clickCutAction);
    connect(m_copyAction, &QAction::triggered, this, &TextEditor::clickCopyAction);
    connect(m_pasteAction, &QAction::triggered, this, &TextEditor::clickPasteAction);
    connect(m_deleteAction, &QAction::triggered, this, &TextEditor::clickDeleteAction);
    connect(m_selectAllAction, &QAction::triggered, this, &TextEditor::selectAll);
    connect(m_findAction, &QAction::triggered, this, &TextEditor::clickFindAction);
    connect(m_replaceAction, &QAction::triggered, this, &TextEditor::clickReplaceAction);
    connect(m_jumpLineAction, &QAction::triggered, this, &TextEditor::clickJumpLineAction);
    connect(m_fullscreenAction, &QAction::triggered, this, &TextEditor::clickFullscreenAction);
    connect(m_exitFullscreenAction, &QAction::triggered, this, &TextEditor::clickFullscreenAction);
    connect(m_enableReadOnlyModeAction, &QAction::triggered, this, &TextEditor::toggleReadOnlyMode);
    connect(m_disableReadOnlyModeAction, &QAction::triggered, this, &TextEditor::toggleReadOnlyMode);
    connect(m_openInFileManagerAction, &QAction::triggered, this, &TextEditor::clickOpenInFileManagerAction);
    connect(m_toggleCommentAction, &QAction::triggered, this, &TextEditor::toggleComment);

    // Init convert case sub menu.
    m_haveWordUnderCursor = false;
    m_convertCaseMenu = new QMenu(tr("Change Case"));
    m_upcaseAction = new QAction(tr("Upper Case"), this);
    m_downcaseAction = new QAction(tr("Lower Case"), this);
    m_capitalizeAction = new QAction(tr("Capitalize"), this);

    m_convertCaseMenu->addAction(m_upcaseAction);
    m_convertCaseMenu->addAction(m_downcaseAction);
    m_convertCaseMenu->addAction(m_capitalizeAction);

    connect(m_upcaseAction, &QAction::triggered, this, &TextEditor::upcaseWord);
    connect(m_downcaseAction, &QAction::triggered, this, &TextEditor::downcaseWord);
    connect(m_capitalizeAction, &QAction::triggered, this, &TextEditor::capitalizeWord);

    m_canUndo = false;
    m_canRedo = false;

    connect(this, &TextEditor::undoAvailable, this, [=] (bool undoIsAvailable) {
        m_canUndo = undoIsAvailable;
    });
    connect(this, &TextEditor::redoAvailable, this, [=] (bool redoIsAvailable) {
        m_canRedo = redoIsAvailable;
    });

    // Init scroll animation.
    m_scrollAnimation = new QPropertyAnimation(verticalScrollBar(), "value");
    m_scrollAnimation->setEasingCurve(QEasingCurve::InOutExpo);
    m_scrollAnimation->setDuration(300);

    connect(m_scrollAnimation, &QPropertyAnimation::finished, this, &TextEditor::handleScrollFinish, Qt::QueuedConnection);

    // Monitor cursor mark status to update in line number area.
    connect(this, &TextEditor::cursorMarkChanged, this, &TextEditor::handleCursorMarkChanged);

    // configure content area
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &TextEditor::adjustScrollbarMargins, Qt::QueuedConnection);
}

int TextEditor::getCurrentLine()
{
    return textCursor().blockNumber() + 1;
}

int TextEditor::getCurrentColumn()
{
    return textCursor().columnNumber();
}

int TextEditor::getPosition()
{
    return textCursor().position();
}

int TextEditor::getScrollOffset()
{
    QScrollBar *scrollbar = verticalScrollBar();

    return scrollbar->value();
}

void TextEditor::forwardChar()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::NextCharacter);
    }
}

void TextEditor::backwardChar()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::PreviousCharacter);
    }
}

void TextEditor::forwardWord()
{
    QTextCursor cursor = textCursor();

    if (m_cursorMark) {
        // cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
    } else {
        // cursor.setPosition(getNextWordPosition(cursor, QTextCursor::MoveAnchor), QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEditor::backwardWord()
{
    QTextCursor cursor = textCursor();

    if (m_cursorMark) {
        // cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
    } else {
        // cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::MoveAnchor), QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEditor::forwardPair()
{
    // Record cursor and seleciton position before move cursor.
    int actionStartPos = textCursor().position();
    int selectionStartPos = textCursor().selectionStart();
    int selectionEndPos = textCursor().selectionEnd();

    // Because find always search start from selection end position.
    // So we need clear selection to make search start from cursor.
    QTextCursor removeSelectionCursor = textCursor();
    removeSelectionCursor.clearSelection();
    setTextCursor(removeSelectionCursor);

    // Start search.
    if (find(QRegExp("[\"'>)}]"))) {
        int findPos = textCursor().position();

        QTextCursor cursor = textCursor();
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
        } else {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
        }

        setTextCursor(cursor);
    }
}

void TextEditor::backwardPair()
{
    // Record cursor and seleciton position before move cursor.
    int actionStartPos = textCursor().position();
    int selectionStartPos = textCursor().selectionStart();
    int selectionEndPos = textCursor().selectionEnd();

    // Because find always search start from selection end position.
    // So we need clear selection to make search start from cursor.
    QTextCursor removeSelectionCursor = textCursor();
    removeSelectionCursor.clearSelection();
    setTextCursor(removeSelectionCursor);

    QTextDocument::FindFlags options;
    options |= QTextDocument::FindBackward;

    if (find(QRegExp("[\"'<({]"), options)) {
        QTextCursor cursor = textCursor();
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);

        int findPos = cursor.position();

        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
            setTextCursor(cursor);
        } else {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
            setTextCursor(cursor);
        }
    }
}

void TextEditor::moveToStart()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Start);
    }
}

void TextEditor::moveToEnd()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::End);
    }
}

void TextEditor::moveToStartOfLine()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::StartOfBlock);
    }
}

void TextEditor::moveToEndOfLine()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::EndOfBlock);
    }
}

void TextEditor::moveToLineIndentation()
{
    // Init cursor and move type.
    QTextCursor cursor = textCursor();
    auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

    // Get line start position.
    cursor.movePosition(QTextCursor::StartOfBlock, moveMode);
    int startColumn = cursor.columnNumber();

    // Get line end position.
    cursor.movePosition(QTextCursor::EndOfBlock, moveMode);
    int endColumn = cursor.columnNumber();

    // Move to line start first.
    cursor.movePosition(QTextCursor::StartOfBlock, moveMode);

    // Move to first non-blank char of line.
    int column = startColumn;
    while (column < endColumn) {
        QChar currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

        if (!currentChar.isSpace()) {
            cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
            break;
        } else {
            cursor.movePosition(QTextCursor::NextCharacter, moveMode);
        }

        column++;
    }

    setTextCursor(cursor);
}

void TextEditor::nextLine()
{
    if (toPlainText().isEmpty())
        return;

    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Down);
    }
}

void TextEditor::prevLine()
{
    if (toPlainText().isEmpty())
        return;

    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Up, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Up);
    }
}

void TextEditor::moveCursorNoBlink(QTextCursor::MoveOperation operation, QTextCursor::MoveMode mode)
{
    // Function moveCursorNoBlink will blink cursor when move cursor.
    // But function movePosition won't, so we use movePosition to avoid that cursor link when moving cursor.
    QTextCursor cursor = textCursor();
    cursor.movePosition(operation, mode);
    setTextCursor(cursor);
}

void TextEditor::jumpToLine(int line, bool keepLineAtCenter)
{
    QTextCursor cursor(document()->findBlockByNumber(line - 1)); // line - 1 because line number starts from 0

    // Update cursor.
    setTextCursor(cursor);

    if (keepLineAtCenter) {
        keepCurrentLineAtCenter();
    }
}

void TextEditor::newline()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.insertText("\n");
    setTextCursor(cursor);
}

void TextEditor::openNewlineAbove()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.insertText("\n");
    cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);

    setTextCursor(cursor);
}

void TextEditor::openNewlineBelow()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    moveCursorNoBlink(QTextCursor::EndOfBlock);
    textCursor().insertText("\n");
}

void TextEditor::moveLineDownUp(bool up)
{
    QTextCursor cursor = textCursor();
    QTextCursor move = cursor;
    bool hasSelection = cursor.hasSelection();

    // this opens folded items instead of destroying them
    move.setVisualNavigation(false);
    move.beginEditBlock();

    if (hasSelection) {
        move.setPosition(cursor.selectionStart());
        move.movePosition(QTextCursor::StartOfBlock);
        move.setPosition(cursor.selectionEnd(), QTextCursor::KeepAnchor);
        move.movePosition(move.atBlockStart() ? QTextCursor::Left: QTextCursor::EndOfBlock,
                          QTextCursor::KeepAnchor);
    } else {
        move.movePosition(QTextCursor::StartOfBlock);
        move.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    }
    const QString text = move.selectedText();

    move.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    move.removeSelectedText();

    if (up) {
        move.movePosition(QTextCursor::PreviousBlock);
        move.insertBlock();
        move.movePosition(QTextCursor::Left);
    } else {
        move.movePosition(QTextCursor::EndOfBlock);
        if (move.atBlockStart()) { // empty block
            move.movePosition(QTextCursor::NextBlock);
            move.insertBlock();
            move.movePosition(QTextCursor::Left);
        } else {
            move.insertBlock();
        }
    }

    int start = move.position();
    move.clearSelection();
    move.insertText(text);
    int end = move.position();

    if (hasSelection) {
        move.setPosition(end);
        move.setPosition(start, QTextCursor::KeepAnchor);
    } else {
        move.setPosition(start);
    }

    move.endEditBlock();
    setTextCursor(move);
}

void TextEditor::scrollLineUp()
{
    QScrollBar *scrollbar = verticalScrollBar();

    scrollbar->setValue(scrollbar->value() - 1);

    if (cursorRect().y() > rect().height() - fontMetrics().height()) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Up, moveMode);
        setTextCursor(cursor);
    }
}

void TextEditor::scrollLineDown()
{
    QScrollBar *scrollbar = verticalScrollBar();

    scrollbar->setValue(scrollbar->value() + 1);

    if (cursorRect().y() < 0) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Down, moveMode);
        setTextCursor(cursor);
    }
}

void TextEditor::scrollUp()
{
    QScrollBar *scrollbar = verticalScrollBar();

    int lines = rect().height() / fontMetrics().height();

    scrollbar->setValue(scrollbar->value() + lines);

    if (scrollbar->value() >= getCurrentLine()) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        int line = scrollbar->value();
        QTextCursor lineCursor(document()->findBlockByLineNumber(line - 1)); // line - 1 because line number starts from 0

        QTextCursor cursor = textCursor();
        cursor.setPosition(lineCursor.position(), moveMode);
        setTextCursor(cursor);
    }
}

void TextEditor::scrollDown()
{
    QScrollBar *scrollbar = verticalScrollBar();

    int lines = rect().height() / fontMetrics().height();

    scrollbar->setValue(scrollbar->value() - lines);

    auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

    int line = scrollbar->value() + lines;
    QTextCursor lineCursor(document()->findBlockByLineNumber(line - 1)); // line - 1 because line number starts from 0

    QTextCursor cursor = textCursor();
    cursor.setPosition(lineCursor.position(), moveMode);
    setTextCursor(cursor);
}

void TextEditor::duplicateLine()
{
    if (textCursor().hasSelection()) {
        bool cursorAtSelectionStart = (textCursor().position() == textCursor().selectionStart());

        // Rember current line's column number.
        int column = textCursor().columnNumber();

        int startPos = textCursor().selectionStart();
        int endPos = textCursor().selectionEnd();

        // Expand selection to lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);

        // Get selection lines content.
        QString selectionLines = cursor.selectedText();

        // Duplicate copy lines.
        if (cursorAtSelectionStart) {
            // Insert duplicate lines.
            cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
            cursor.insertText("\n");
            cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
            cursor.insertText(selectionLines);

            // Restore cursor's column.
            cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
        } else {
            // Insert duplicate lines.
            cursor.setPosition(endCursor.position(), QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor);
            cursor.insertText(selectionLines);
            cursor.insertText("\n");

            // Restore cursor's column.
            cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
        }

        // Update cursor.
        setTextCursor(cursor);

        // Clean mark flag anyway.
        unsetMark();
    } else {
        // Rember current line's column number.
        int column = textCursor().columnNumber();

        // Get current line's content.
        QTextCursor cursor(textCursor().block());
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString text = cursor.selectedText();

        // Copy current line.
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
        cursor.insertText("\n");
        cursor.insertText(text);

        // Restore cursor's column.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);

        // Update cursor.
        setTextCursor(cursor);
    }
}

void TextEditor::copyLines()
{
    // Record current cursor and build copy cursor.
    QTextCursor currentCursor = textCursor();
    QTextCursor copyCursor = textCursor();

    if (textCursor().hasSelection()) {
        // Sort selection bound cursors.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Selectoin multi-lines.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        copyCursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        copyCursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        // popupNotify(tr("已经拷贝选中行到剪切板"));
    } else {
        // Selection current line.
        copyCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        copyCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        // popupNotify(tr("已经拷贝当前行到剪切板"));
    }

    // Copy lines to system clipboard.
    setTextCursor(copyCursor);
    copySelectedText();

    // Reset cursor before copy lines.
    copyCursor.setPosition(currentCursor.position(), QTextCursor::MoveAnchor);
    setTextCursor(copyCursor);
}

void TextEditor::cutlines()
{
    // Record current cursor and build copy cursor.
    QTextCursor currentCursor = textCursor();
    QTextCursor copyCursor = textCursor();

    if (textCursor().hasSelection()) {
        // Sort selection bound cursors.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Selectoin multi-lines.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        copyCursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        copyCursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        // popupNotify(tr("已经剪切选中行到剪切板"));
    } else {
        // Selection current line.
        copyCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        copyCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        // popupNotify(tr("已经剪切当前行到剪切板"));
    }

    // Copy lines to system clipboard.
    setTextCursor(copyCursor);
    cutSelectedText();

    // Reset cursor before copy lines.
    copyCursor.setPosition(currentCursor.position(), QTextCursor::MoveAnchor);
    setTextCursor(copyCursor);
}

void TextEditor::joinLines()
{
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Expand selection to multi-lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        // Select multi-lines.
        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        // Remove selected lines.
        QString selectedLines = cursor.selectedText();
        cursor.removeSelectedText();

        // Insert line with join actoin.
        // Because function `selectedText' will use Unicode char U+2029 instead \n,
        // so we need replace Unicode char U+2029, not replace char '\n'.
        cursor.insertText(selectedLines.replace(QChar(0x2029), " "));

        setTextCursor(cursor);
    } else {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
        cursor.insertText(" ");
        cursor.deleteChar();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        setTextCursor(cursor);
    }

    tryUnsetMark();
}

void TextEditor::killLine()
{
    if (tryUnsetMark()) {
        return;
    }

    // Remove selection content if has selection.
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        // Get current line content.
        QTextCursor selectionCursor = textCursor();
        selectionCursor.movePosition(QTextCursor::StartOfBlock);
        selectionCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString text = selectionCursor.selectedText();

        bool isEmptyLine = text.size() == 0;
        bool isBlankLine = text.trimmed().size() == 0;

        QTextCursor cursor = textCursor();
        // Join next line if current line is empty or cursor at end of line.
        if (isEmptyLine || textCursor().atBlockEnd()) {
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
            cursor.deleteChar();
        }
        // Kill whole line if current line is blank line.
        else if (isBlankLine && textCursor().atBlockStart()) {
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            cursor.deleteChar();
        }
        // Otherwise kill rest content of line.
        else {
            cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }

        // Update cursor.
        setTextCursor(cursor);
    }
}

void TextEditor::killCurrentLine()
{
    if (tryUnsetMark()) {
        return;
    }

    QTextCursor cursor = textCursor();

    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

    QString text = cursor.selectedText();
    bool isBlankLine = text.trimmed().size() == 0;

    cursor.removeSelectedText();
    if (isBlankLine) {
        cursor.deleteChar();
    }

    setTextCursor(cursor);
}

void TextEditor::killBackwardWord()
{
    tryUnsetMark();

    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        //  QTextCursor cursor = textCursor();
        //  cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        //  cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        //  cursor.removeSelectedText();
        //  setTextCursor(cursor);

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        setTextCursor(cursor);
    }
}

void TextEditor::killForwardWord()
{
    tryUnsetMark();

    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        //  QTextCursor cursor = textCursor();
        //  cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        //  cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        //  cursor.removeSelectedText();
        //  setTextCursor(cursor);

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        setTextCursor(cursor);
    }
}

void TextEditor::indentText()
{
    // Stop mark if mark is set.
    tryUnsetMark();
    hideCursorBlink();

    QTextCursor cursor = textCursor();

    if (cursor.hasSelection()) {
        QTextBlock block = document()->findBlock(cursor.selectionStart());
        QTextBlock end = document()->findBlock(cursor.selectionEnd()).next();

        cursor.beginEditBlock();

        while (block != end) {
            QString speaces(m_tabSpaceNumber, ' ');
            cursor.setPosition(block.position());
            cursor.insertText(speaces);
            block = block.next();
        }

        cursor.endEditBlock();
    } else {
        cursor.beginEditBlock();

        int indent = m_tabSpaceNumber - (cursor.positionInBlock() % m_tabSpaceNumber);
        QString spaces(indent, ' ');
        cursor.insertText(spaces);

        cursor.endEditBlock();
    }

    showCursorBlink();
}

void TextEditor::unindentText()
{
    // Stop mark if mark is set.
    tryUnsetMark();
    hideCursorBlink();

    QTextCursor cursor = textCursor();
    QTextBlock block;
    QTextBlock end;

    if (cursor.hasSelection()) {
        block = document()->findBlock(cursor.selectionStart());
        end = document()->findBlock(cursor.selectionEnd()).next();
    } else {
        block = cursor.block();
        end = block.next();
    }

    cursor.beginEditBlock();

    while (block != end) {
        cursor.setPosition(block.position());

        if (document()->characterAt(cursor.position()) == '\t') {
            cursor.deleteChar();
        } else {
            int pos = 0;

            while (document()->characterAt(cursor.position()) == ' ' &&
                   pos < m_tabSpaceNumber){
                pos++;
                cursor.deleteChar();
            }
        }

        block = block.next();
    }

    cursor.endEditBlock();
    showCursorBlink();
}

void TextEditor::setTabSpaceNumber(int number)
{
    m_tabSpaceNumber = number;
}

void TextEditor::upcaseWord()
{
    tryUnsetMark();

    convertWordCase(UPPER);
}

void TextEditor::downcaseWord()
{
    tryUnsetMark();

    convertWordCase(LOWER);
}

void TextEditor::capitalizeWord()
{
    tryUnsetMark();

    convertWordCase(CAPITALIZE);
}

void TextEditor::transposeChar()
{
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.clearSelection();

    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QString nextChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    QString prevChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.insertText(nextChar);
    cursor.insertText(prevChar);

    if (!nextChar.isEmpty()) {
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEditor::handleCursorMarkChanged(bool mark, QTextCursor cursor)
{
    if (mark) {
        m_markStartLine = cursor.blockNumber() + 1;
    } else {
        m_markStartLine = -1;
    }

    lineNumberArea->update();
}

void TextEditor::convertWordCase(ConvertCase convertCase)
{
    if (textCursor().hasSelection()) {
        QString text = textCursor().selectedText();

        if (convertCase == UPPER) {
            textCursor().insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            textCursor().insertText(text.toLower());
        } else {
            textCursor().insertText(capitalizeText(text));
        }
    } else {
        QTextCursor cursor;

        // Move cursor to mouse position first. if have word under mouse pointer.
        if (m_haveWordUnderCursor) {
            setTextCursor(m_wordUnderPointerCursor);
        }

        cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);

        QString text = cursor.selectedText();
        if (convertCase == UPPER) {
            cursor.insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            cursor.insertText(text.toLower());
        } else {
            cursor.insertText(capitalizeText(text));
        }

        setTextCursor(cursor);

        m_haveWordUnderCursor = false;
    }
}

QString TextEditor::capitalizeText(QString text)
{
    QString newText = text.toLower();
    QChar currentChar;
    for (int i = 0; i < newText.size(); i++) {
        currentChar = newText.at(i);
        if (!currentChar.isSpace() && !m_wordSepartors.contains(QString(currentChar))) {
            newText.replace(i, 1, currentChar.toUpper());
            break;
        }
    }

    return newText;
}

void TextEditor::keepCurrentLineAtCenter()
{
    QScrollBar *scrollbar = verticalScrollBar();

    int currentLine = cursorRect().top() / cursorRect().height();
    int halfEditorLines = rect().height() / 2 / cursorRect().height();
    scrollbar->setValue(scrollbar->value() + currentLine - halfEditorLines);
}

void TextEditor::scrollToLine(int scrollOffset, int row, int column)
{
    // Save cursor postion.
    m_restoreRow = row;
    m_restoreColumn = column;

    // Start scroll animation.
    m_scrollAnimation->setStartValue(verticalScrollBar()->value());
    m_scrollAnimation->setEndValue(scrollOffset);
    m_scrollAnimation->start();
}

void TextEditor::setFontFamily(QString name)
{
    // Update font.
    m_fontName = name;
    updateFont();
}

void TextEditor::setFontSize(int size)
{
    // Update font.
    m_fontSize = size;
    updateFont();

    // Update line number after adjust font size.
    updateLineNumber();
}

void TextEditor::updateFont()
{
    QFont font = document()->defaultFont();
    font.setFixedPitch(true);
    font.setPointSize(m_fontSize);
    font.setFamily(m_fontName);
    setFont(font);
}

void TextEditor::replaceAll(const QString &replaceText, const QString &withText)
{
    if (replaceText.isEmpty()) {
        return;
    }

    QTextDocument::FindFlags flags;
    flags &= QTextDocument::FindCaseSensitively;

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);

    QTextCursor startCursor = textCursor();
    startCursor.beginEditBlock();

    while (1) {
        cursor = document()->find(replaceText, cursor, flags);

        if (!cursor.isNull()) {
            cursor.insertText(withText);
        } else {
            break;
        }
    }

    startCursor.endEditBlock();
    setTextCursor(startCursor);
}

void TextEditor::replaceNext(const QString &replaceText, const QString &withText)
{
    if (replaceText.isEmpty() ||
        !m_cursorKeywordSelection.cursor.hasSelection()) {
        // 无限替换的根源
        highlightKeyword(replaceText, getPosition());
        return;
    }

    QTextCursor cursor = textCursor();
    cursor.setPosition(m_cursorKeywordSelection.cursor.position() - replaceText.size());
    cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, replaceText.size());
    cursor.insertText(withText);

    // Update cursor.
    setTextCursor(cursor);
    highlightKeyword(replaceText, getPosition());
}

void TextEditor::replaceRest(const QString &replaceText, const QString &withText)
{
    // If replace text is nothing, don't do replace action.
    if (replaceText.isEmpty()) {
        return;
    }

    QTextDocument::FindFlags flags;
    flags &= QTextDocument::FindCaseSensitively;

    QTextCursor cursor = textCursor();

    QTextCursor startCursor = textCursor();
    startCursor.beginEditBlock();

    while (1) {
        cursor = document()->find(replaceText, cursor, flags);

        if (!cursor.isNull()) {
            cursor.insertText(withText);
        } else {
            break;
        }
    }

    startCursor.endEditBlock();
    setTextCursor(startCursor);
}

bool TextEditor::findKeywordForward(QString keyword)
{
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
        setTextCursor(cursor);

        QTextDocument::FindFlags options;
        options &= QTextDocument::FindCaseSensitively;

        bool foundOne = find(keyword, options);

        cursor.setPosition(endPos, QTextCursor::MoveAnchor);
        cursor.setPosition(startPos, QTextCursor::KeepAnchor);
        setTextCursor(cursor);

        return foundOne;
    } else {
        QTextCursor recordCursor = textCursor();

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
        setTextCursor(cursor);

        QTextDocument::FindFlags options;
        options &= QTextDocument::FindCaseSensitively;

        bool foundOne = find(keyword, options);

        setTextCursor(recordCursor);

        return foundOne;
    }
}

void TextEditor::removeKeywords()
{
    m_cursorKeywordSelection.cursor = textCursor();
    m_cursorKeywordSelection.cursor.clearSelection();

    m_keywordSelections.clear();

    updateHighlightLineSelection();

    renderAllSelections();

    setFocus();
}

void TextEditor::highlightKeyword(QString keyword, int position)
{
    updateKeywordSelections(keyword);
    updateCursorKeywordSelection(position, true);
    updateHighlightLineSelection();
    renderAllSelections();
}

void TextEditor::updateCursorKeywordSelection(int position, bool findNext)
{
    bool findOne = setCursorKeywordSeletoin(position, findNext);

    if (!findOne) {
        if (findNext) {
            // Clear keyword if keyword not match anything.
            if (!setCursorKeywordSeletoin(0, findNext)) {
                m_cursorKeywordSelection.cursor = textCursor();

                m_keywordSelections.clear();
                renderAllSelections();
            }
        } else {
            QTextCursor cursor = textCursor();
            cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);

            m_cursorKeywordSelection.cursor.clearSelection();
            setCursorKeywordSeletoin(cursor.position(), findNext);
        }
    }
}

void TextEditor::updateHighlightLineSelection()
{
    QTextEdit::ExtraSelection selection;

    selection.format.setBackground(m_currentLineColor);
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();

    m_currentLineSelection = selection;
}

void TextEditor::updateKeywordSelections(QString keyword)
{
    // Clear keyword selections first.
    m_keywordSelections.clear();

    // Update selections with keyword.
    if (!keyword.isEmpty()) {
        QTextCursor cursor(document());

        QTextDocument::FindFlags flags;
        flags &= QTextDocument::FindCaseSensitively;
        cursor = document()->find(keyword, cursor, flags);

        while (!cursor.isNull()) {
            QTextEdit::ExtraSelection extra;
            QBrush bgBrush(m_selectionBgColor);
            QBrush fgBrush(m_selectionColor);
            extra.format.setProperty(QTextFormat::BackgroundBrush, bgBrush);
            extra.format.setProperty(QTextFormat::ForegroundBrush, fgBrush);
            extra.cursor = cursor;

            cursor = document()->find(keyword, cursor, flags);
            m_keywordSelections.append(extra);
        }

        setExtraSelections(m_keywordSelections);
    }
}

void TextEditor::renderAllSelections()
{
    QList<QTextEdit::ExtraSelection> selections;

    selections.append(m_currentLineSelection);
    selections.append(m_keywordSelections);
    selections.append(m_cursorKeywordSelection);
    selections.append(m_wordUnderCursorSelection);

    setExtraSelections(selections);
}

void TextEditor::keyPressEvent(QKeyEvent *keyEvent)
{
    const QString &key = Utils::getKeyshortcut(keyEvent);

    if (m_readOnlyMode) {
        if (key == "J") {
            nextLine();
        } else if (key == "K") {
            prevLine();
        } else if (key == ",") {
            moveToEnd();
        } else if (key == ".") {
            moveToStart();
        } else if (key == "H") {
            backwardChar();
        } else if (key == "L") {
            forwardChar();
        } else if (key == "Space") {
            scrollUp();
        } else if (key == "V") {
            scrollDown();
        } else if (key == "F") {
            forwardWord();
        } else if (key == "B") {
            backwardWord();
        } else if (key == "A") {
            moveToStartOfLine();
        } else if (key == "E") {
            moveToEndOfLine();
        } else if (key == "M") {
            moveToLineIndentation();
        } else if (key == "Q") {
            toggleReadOnlyMode();
        } else if (key == "Shfit+J") {
            scrollLineUp();
        } else if (key == "Shift+K") {
            scrollLineDown();
        } else if (key == "P") {
            forwardPair();
        } else if (key == "N") {
            backwardPair();
        } else if (key == "Shift+:") {
            copyLines();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglereadonlymode")) {
            toggleReadOnlyMode();
        }
    } else {
        if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "indentline")) {
            indentText();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backindentline")) {
            unindentText();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardchar")) {
            forwardChar();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardchar")) {
            backwardChar();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardword")) {
            forwardWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardword")) {
            backwardWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "nextline")) {
            nextLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "prevline")) {
            prevLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "newline") || key == "Return") {
            newline();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "opennewlineabove")) {
            openNewlineAbove();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "opennewlinebelow")) {
            openNewlineBelow();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "duplicateline")) {
            duplicateLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killline")) {
            killLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killcurrentline")) {
            killCurrentLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "swaplineup")) {
            moveLineDownUp(true);
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "swaplinedown")) {
            moveLineDownUp(false);
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolllineup")) {
            scrollLineUp();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolllinedown")) {
            scrollLineDown();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrollup")) {
            scrollUp();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolldown")) {
            scrollDown();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetoendofline")) {
            moveToEndOfLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetostartofline")) {
            moveToStartOfLine();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetostart")) {
            moveToStart();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetoend")) {
            moveToEnd();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetolineindentation")) {
            moveToLineIndentation();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "upcaseword")) {
            upcaseWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "downcaseword")) {
            downcaseWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "capitalizeword")) {
            capitalizeWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killbackwardword")) {
            killBackwardWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killforwardword")) {
            killForwardWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardpair")) {
            forwardPair();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardpair")) {
            backwardPair();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "transposechar")) {
            transposeChar();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "selectall")) {
            selectAll();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "copy")) {
            copySelectedText();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "cut")) {
            cutSelectedText();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "paste")) {
            pasteText();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "setmark")) {
            setMark();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "exchangemark")) {
            exchangeMark();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "copylines")) {
            copyLines();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "cutlines")) {
            cutlines();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "joinlines")) {
            joinLines();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglereadonlymode")) {
            toggleReadOnlyMode();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglecomment")) {
            toggleComment();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "undo")) {
            QPlainTextEdit::undo();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "redo")) {
            QPlainTextEdit::redo();
        } else if (key == "Esc") {
            emit pressEsc();
        } else {
            // Post event to window widget if key match window key list.
            for (auto option : m_settings->settings->group("shortcuts.window")->options()) {
                if (key == m_settings->settings->option(option->key())->value().toString()) {
                    keyEvent->ignore();
                    return;
                }
            }

            // Post event to window widget if match Alt+0 ~ Alt+9
            QRegularExpression re("^Alt\\+\\d");
            QRegularExpressionMatch match = re.match(key);
            if (match.hasMatch()) {
                keyEvent->ignore();
                return;
            }

            // Text editor handle key self.
            QPlainTextEdit::keyPressEvent(keyEvent);
        }
    }
}

void TextEditor::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const int deltaY = e->angleDelta().y();

        if (deltaY < 0) {
            qobject_cast<Window *>(this->window())->decrementFontSize();
        } else {
            qobject_cast<Window *>(this->window())->incrementFontSize();
        }

        return;
    }

    QPlainTextEdit::wheelEvent(e);
}

void TextEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    // Init.
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), m_backgroundColor);

    QColor splitLineColor;
    if (QColor(m_backgroundColor).lightness() < 128) {
        splitLineColor = "#ffffff";
    } else {
        splitLineColor = "#000000";
    }
    splitLineColor.setAlphaF(0.05);
    painter.fillRect(QRect(event->rect().x() + event->rect().width() - 1, event->rect().y(), 1, event->rect().height()), splitLineColor);

    // Update line number.
    QTextBlock block = firstVisibleBlock();
    int top = (int) blockBoundingGeometry(block).translated(contentOffset()).top();
    int bottom = top + (int) blockBoundingRect(block).height();
    int linenumber = block.blockNumber();

    Utils::setFontSize(painter, document()->defaultFont().pointSize() - 2);
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            if (linenumber + 1 == m_markStartLine) {
                painter.setPen(m_regionMarkerColor);
            } else {
                painter.setPen(m_lineNumbersColor);
            }
            painter.drawText(0,
                             top,
                             lineNumberArea->width(),
                             blockBoundingRect(block).height(),
                             Qt::AlignTop | Qt::AlignHCenter,
                             QString::number(linenumber + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + (int) blockBoundingRect(block).height();

        ++linenumber;
    }
}

void TextEditor::contextMenuEvent(QContextMenuEvent *event)
{
    m_rightMenu->clear();

    QString wordAtCursor = getWordAtMouse();

    QTextCursor selectionCursor = textCursor();
    selectionCursor.movePosition(QTextCursor::StartOfBlock);
    selectionCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    QString text = selectionCursor.selectedText();

    bool isBlankLine = text.trimmed().size() == 0;

    if (m_canUndo) {
        m_rightMenu->addAction(m_undoAction);
    }
    if (m_canRedo) {
        m_rightMenu->addAction(m_redoAction);
    }
    m_rightMenu->addSeparator();
    if (textCursor().hasSelection()) {
        m_rightMenu->addAction(m_cutAction);
        m_rightMenu->addAction(m_copyAction);
    } else {
        // Just show copy/cut menu item when cursor rectangle contain moue pointer coordinate.
        m_haveWordUnderCursor = highlightWordUnderMouse(event->pos());
        if (m_haveWordUnderCursor) {
            if (wordAtCursor != "") {
                m_rightMenu->addAction(m_cutAction);
                m_rightMenu->addAction(m_copyAction);
            }
        }
    }
    if (canPaste()) {
        m_rightMenu->addAction(m_pasteAction);
    }

    if (wordAtCursor != "") {
        m_rightMenu->addAction(m_deleteAction);
    }
    if (toPlainText() != "") {
        m_rightMenu->addAction(m_selectAllAction);
    }
    m_rightMenu->addSeparator();
    if (toPlainText() != "") {
        m_rightMenu->addAction(m_findAction);
        m_rightMenu->addAction(m_replaceAction);
        m_rightMenu->addAction(m_jumpLineAction);
        m_rightMenu->addSeparator();
    }
    if (wordAtCursor != "") {
        m_rightMenu->addMenu(m_convertCaseMenu);
    }

    // intelligent judge whether to support comments.
    const auto def = m_repository.definitionForFileName(QFileInfo(filepath).fileName());
    if (!toPlainText().isEmpty() &&
        (textCursor().hasSelection() || !isBlankLine) &&
        !def.filePath().isEmpty()) {
        m_rightMenu->addAction(m_toggleCommentAction);
    }

    m_rightMenu->addSeparator();
    if (m_readOnlyMode) {
        m_rightMenu->addAction(m_disableReadOnlyModeAction);
    } else {
        m_rightMenu->addAction(m_enableReadOnlyModeAction);
    }
    m_rightMenu->addAction(m_openInFileManagerAction);
    m_rightMenu->addSeparator();
    if (static_cast<Window*>(this->window())->isFullScreen()) {
        m_rightMenu->addAction(m_exitFullscreenAction);
    } else {
        m_rightMenu->addAction(m_fullscreenAction);
    }

    m_rightMenu->exec(event->globalPos());
}

void TextEditor::highlightCurrentLine()
{
    updateHighlightLineSelection();
    renderAllSelections();

    // Adjust scrollbar margins if reach last line.
    // FIXME(rekols): Temporarily do not need this function.
    // if (getCurrentLine() == blockCount()) {
    //     // Adjust y coordinate up one line height.
    //     m_scrollbarMargin = fontMetrics().height();

    //     adjustScrollbarMargins();

    //     // NOTE: do nextLine make scrollbar adjust y coordinate.
    //     nextLine();
    // } else {
    //     m_scrollbarMargin = 0;

    //     adjustScrollbarMargins();
    // }

    adjustScrollbarMargins();

    // Keep current line at visible area.
    // if (cursorRect().top() + fontMetrics().height() >= rect().height()) {
    //     scrollLineUp();
    // }
}

void TextEditor::updateLineNumber()
{
    // Update line number painter.
    lineNumberArea->setFixedWidth(QString("%1").arg(blockCount()).size() * fontMetrics().width("9") + m_lineNumberPaddingX * 2);
}

void TextEditor::handleScrollFinish()
{
    // Restore cursor postion.
    jumpToLine(m_restoreRow, false);

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, m_restoreColumn);

    // Update cursor.
    setTextCursor(cursor);
}

void TextEditor::handleUpdateRequest(const QRect &rect, int dy)
{
    if (dy) {
        lineNumberArea->scroll(0, dy);
    } else {
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    }
}

bool TextEditor::setCursorKeywordSeletoin(int position, bool findNext)
{
    int offsetLines = 3;

    if (findNext) {
        for (int i = 0; i < m_keywordSelections.size(); i++) {
            if (m_keywordSelections[i].cursor.position() > position) {
                m_cursorKeywordSelection.cursor = m_keywordSelections[i].cursor;

                QBrush bgBrush(m_searchHighlightBgColor);
                QBrush fgBrush(m_searchHighlightColor);
                m_cursorKeywordSelection.format.setProperty(QTextFormat::ForegroundBrush, fgBrush);
                m_cursorKeywordSelection.format.setProperty(QTextFormat::BackgroundBrush, bgBrush);

                jumpToLine(m_keywordSelections[i].cursor.blockNumber() + offsetLines, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(m_keywordSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);

                return true;
            }
        }
    } else {
        for (int i = m_keywordSelections.size() - 1; i >= 0; i--) {
            if (m_keywordSelections[i].cursor.position() < position) {
                m_cursorKeywordSelection.cursor = m_keywordSelections[i].cursor;

                QBrush bgBrush(m_searchHighlightBgColor);
                QBrush fgBrush(m_searchHighlightColor);
                m_cursorKeywordSelection.format.setProperty(QTextFormat::ForegroundBrush, fgBrush);
                m_cursorKeywordSelection.format.setProperty(QTextFormat::BackgroundBrush, bgBrush);

                jumpToLine(m_keywordSelections[i].cursor.blockNumber() + offsetLines, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(m_keywordSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);

                return true;
            }
        }
    }

    return false;
}

void TextEditor::setThemeWithPath(const QString &path)
{
    const KSyntaxHighlighting::Theme theme = m_repository.theme("");
    setTheme(theme, path);
}

void TextEditor::setTheme(const KSyntaxHighlighting::Theme &theme, const QString &path)
{
    QVariantMap jsonMap = Utils::getThemeMapFromPath(path);
    QVariantMap textStylesMap = jsonMap["text-styles"].toMap();
    const QString &themeCurrentLineColor = jsonMap["editor-colors"].toMap()["current-line"].toString();
    const QString textColor = textStylesMap["Normal"].toMap()["text-color"].toString();

    m_backgroundColor = QColor(jsonMap["editor-colors"].toMap()["background-color"].toString());
    m_currentLineColor = QColor(themeCurrentLineColor);
    m_currentLineNumberColor = QColor(jsonMap["editor-colors"].toMap()["current-line-number"].toString());
    m_lineNumbersColor = QColor(jsonMap["editor-colors"].toMap()["line-numbers"].toString());
    m_regionMarkerColor = QColor(textStylesMap["RegionMarker"].toMap()["selected-text-color"].toString());
    m_searchHighlightColor = QColor(jsonMap["editor-colors"].toMap()["search-highlight-color"].toString());
    m_searchHighlightBgColor = QColor(jsonMap["editor-colors"].toMap()["search-highlight-bg-color"].toString());
    m_selectionColor = QColor(textStylesMap["Normal"].toMap()["selected-text-color"].toString());
    m_selectionBgColor = QColor(textStylesMap["Normal"].toMap()["selected-bg-color"].toString());

    const QString &styleSheet = QString("QPlainTextEdit {"
                                        "background-color: %1;"
                                        "color: %2;"
                                        "selection-color: %3;"
                                        "selection-background-color: %4;"
                                        "}").arg(m_backgroundColor.name(), textColor,
                                                 m_selectionColor.name(), m_selectionBgColor.name());
    setStyleSheet(styleSheet);

    if (m_backgroundColor.lightness() < 128) {
        m_highlighter->setTheme(m_repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme));
    } else {
        m_highlighter->setTheme(m_repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme));
    }

    // does not support highlight do not reload
    // when switching theme will be jammed or large files.
    if (m_highlighted) {
        m_highlighter->rehighlight();
    }

    lineNumberArea->update();
    highlightCurrentLine();
}

void TextEditor::loadHighlighter()
{
    const auto def = m_repository.definitionForFileName(QFileInfo(filepath).fileName());

    if (!def.filePath().isEmpty()) {
        const QString &syntaxFile = QFileInfo(QString(":/syntax/%1").arg(QFileInfo(def.filePath()).fileName())).absoluteFilePath();

        QFile file(syntaxFile);
        if (!file.open(QFile::ReadOnly)) {
            qDebug() << "Can't open file" << syntaxFile;
        }

        QXmlStreamReader reader(&file);
        QString singleLineComment;
        QString multiLineCommentStart;
        QString multiLineCommentEnd;

        while (!reader.atEnd()) {
            const auto token = reader.readNext();
            if (token != QXmlStreamReader::StartElement) {
                continue;
            }

            if (reader.name() == "comment") {
                if (reader.attributes().hasAttribute(QStringLiteral("name"))) {
                    QString attrName = reader.attributes().value(QStringLiteral("name")).toString();

                    if (attrName == "singleLine") {
                        singleLineComment = reader.attributes().value(QStringLiteral("start")).toString();
                    } else if (attrName == "multiLine") {
                        multiLineCommentStart = reader.attributes().value(QStringLiteral("start")).toString();
                        multiLineCommentEnd = reader.attributes().value(QStringLiteral("end")).toString();
                    }
                }
            }
        }

        m_commentDefinition.setComments(QString("%1 ").arg(singleLineComment), multiLineCommentStart, multiLineCommentEnd);

        m_highlighter->setDefinition(def);

        file.close();

        m_highlighted = true;
    } else {
        m_highlighted = false;
    }
}

bool TextEditor::highlightWordUnderMouse(QPoint pos)
{
    // Get cursor match mouse pointer coordinate, but cursor maybe not under mouse pointer.
    QTextCursor cursor(cursorForPosition(pos));

    // Get cursor rectangle.
    auto rect = cursorRect(cursor);
    int widthOffset = 10;
    rect.setX(std::max(rect.x() - widthOffset / 2, 0));
    rect.setWidth(rect.width() + widthOffset);

    // Just highlight word under pointer when cursor rectangle contain moue pointer coordinate.
    if ((rect.x() <= pos.x()) &&
        (pos.x() <= rect.x() + rect.width()) &&
        (rect.y() <= pos.y()) &&
        (pos.y() <= rect.y() + rect.height())) {
        // Move back to word bound start postion, and save cursor for convert case.
        m_wordUnderPointerCursor = cursor;
        m_wordUnderPointerCursor.select(QTextCursor::WordUnderCursor);
        m_wordUnderPointerCursor.setPosition(m_wordUnderPointerCursor.anchor(), QTextCursor::MoveAnchor);

        // Update highlight cursor.
        QTextEdit::ExtraSelection selection;

        selection.format.setBackground(m_selectionBgColor);
        selection.format.setForeground(m_selectionColor);
        selection.cursor = cursor;
        selection.cursor.select(QTextCursor::WordUnderCursor);

        m_wordUnderCursorSelection = selection;

        renderAllSelections();

        return true;
    } else {
        return false;
    }
}

void TextEditor::removeHighlightWordUnderCursor()
{
    m_highlightWordCacheCursor = m_wordUnderCursorSelection.cursor;

    QTextEdit::ExtraSelection selection;
    m_wordUnderCursorSelection = selection;

    renderAllSelections();
}

void TextEditor::setSettings(Settings *keySettings)
{
    m_settings = keySettings;
}

void TextEditor::setModified(bool modified)
{
    document()->setModified(modified);
}

void TextEditor::copySelectedText()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(textCursor().selection().toPlainText());
}

void TextEditor::cutSelectedText()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(textCursor().selection().toPlainText());

    QTextCursor cursor = textCursor();
    cursor.removeSelectedText();
    setTextCursor(cursor);

    unsetMark();
}

void TextEditor::pasteText()
{
    QPlainTextEdit::paste();

    unsetMark();
}

void TextEditor::setMark()
{
    bool currentMark = m_cursorMark;
    bool markCursorChanged = false;

    if (m_cursorMark) {
        if (textCursor().hasSelection()) {
            markCursorChanged = true;

            QTextCursor cursor = textCursor();
            cursor.clearSelection();
            setTextCursor(cursor);
        } else {
            m_cursorMark = false;
        }
    } else {
        m_cursorMark = true;
    }

    if (m_cursorMark != currentMark || markCursorChanged) {
        cursorMarkChanged(m_cursorMark, textCursor());
    }
}

void TextEditor::unsetMark()
{
    bool currentMark = m_cursorMark;

    m_cursorMark = false;

    if (m_cursorMark != currentMark) {
        cursorMarkChanged(m_cursorMark, textCursor());
    }
}

bool TextEditor::tryUnsetMark()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        setTextCursor(cursor);

        unsetMark();

        return true;
    } else {
        return false;
    }
}

void TextEditor::exchangeMark()
{
    if (textCursor().hasSelection()) {
        // Record cursor and seleciton position before move cursor.
        int actionStartPos = textCursor().position();
        int selectionStartPos = textCursor().selectionStart();
        int selectionEndPos = textCursor().selectionEnd();

        QTextCursor cursor = textCursor();
        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(selectionEndPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(selectionStartPos, QTextCursor::KeepAnchor);
        }

        setTextCursor(cursor);
    }
}

void TextEditor::saveMarkStatus()
{
    m_cursorMarkStatus = m_cursorMark;
    m_cursorMarkPosition = textCursor().anchor();
}

void TextEditor::restoreMarkStatus()
{
    if (m_cursorMarkStatus) {
        QTextCursor currentCursor = textCursor();

        QTextCursor cursor = textCursor();
        cursor.setPosition(m_cursorMarkPosition, QTextCursor::MoveAnchor);
        cursor.setPosition(currentCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);
    }
}

void TextEditor::clickCutAction()
{
    if (textCursor().hasSelection()) {
        cutSelectedText();
    } else {
        cutWordUnderCursor();
    }
}

void TextEditor::clickCopyAction()
{
    if (textCursor().hasSelection()) {
        copySelectedText();
    } else {
        copyWordUnderCursor();
    }
}

void TextEditor::clickPasteAction()
{
    if (textCursor().hasSelection()) {
        pasteText();
    } else {
        QTextCursor cursor;

        // Move to word cursor if have word around mouse.
        // Otherwise find nearest cursor with mouse click.
        if (m_highlightWordCacheCursor.position() != -1) {
            cursor = textCursor();
            cursor.setPosition(m_highlightWordCacheCursor.position(), QTextCursor::MoveAnchor);
        } else {
            auto pos = mapFromGlobal(m_mouseClickPos);
            cursor = cursorForPosition(pos);
        }

        setTextCursor(cursor);

        pasteText();
    }
}

void TextEditor::clickDeleteAction()
{
    if (textCursor().hasSelection()) {
        textCursor().removeSelectedText();
    } else {
        setTextCursor(m_highlightWordCacheCursor);
        textCursor().removeSelectedText();
    }
}

void TextEditor::clickOpenInFileManagerAction()
{
    DDesktopServices::showFileItem(filepath);
}

void TextEditor::copyWordUnderCursor()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(m_highlightWordCacheCursor.selectedText());
}

void TextEditor::cutWordUnderCursor()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(m_highlightWordCacheCursor.selectedText());

    setTextCursor(m_highlightWordCacheCursor);
    textCursor().removeSelectedText();
}

QString TextEditor::getWordAtCursor()
{
    if (toPlainText().isEmpty()) {
        return "";
    } else {
        QTextCursor cursor = textCursor();
        QChar currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        while (!currentChar.isSpace() && cursor.position() != 0) {
            // while (!currentChar.isSpace() && cursor.position() != 0) {
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
            currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

            if (currentChar == '-') {
                break;
            }
        }

        return cursor.selectedText();
    }
}

QString TextEditor::getWordAtMouse()
{
    if (toPlainText().isEmpty()) {
        return "";
    } else {
        auto pos = mapFromGlobal(QCursor::pos());
        QTextCursor cursor(cursorForPosition(pos));

        // Get cursor rectangle.
        auto rect = cursorRect(cursor);
        int widthOffset = 10;
        rect.setX(std::max(rect.x() - widthOffset / 2, 0));
        rect.setWidth(rect.width() + widthOffset);

        // Just highlight word under pointer when cursor rectangle contain moue pointer coordinate.
        if ((rect.x() <= pos.x()) &&
            (pos.x() <= rect.x() + rect.width()) &&
            (rect.y() <= pos.y()) &&
            (pos.y() <= rect.y() + rect.height())) {
            cursor.select(QTextCursor::WordUnderCursor);

            return cursor.selectedText();
        } else {
            return "";
        }
    }
}

void TextEditor::toggleReadOnlyMode()
{
    if (m_readOnlyMode) {
        m_readOnlyMode = false;

        popupNotify(tr("Read-Only mode is off"));
    } else {
        m_readOnlyMode = true;

        popupNotify(tr("Read-Only mode is on"));
    }
}

void TextEditor::toggleComment()
{
    const auto def = m_repository.definitionForFileName(QFileInfo(filepath).fileName());

    if (!def.filePath().isEmpty()) {
        Comment::unCommentSelection(this, m_commentDefinition);
    } else {
        // do not need to prompt the user.
        // popupNotify(tr("File does not support syntax comments"));
    }
}

int TextEditor::getNextWordPosition(QTextCursor cursor, QTextCursor::MoveMode moveMode)
{
    // FIXME(rekols): if is empty text, it will crash.
    if (toPlainText().isEmpty()) {
        return 0;
    }

    // Move next char first.
    cursor.movePosition(QTextCursor::NextCharacter, moveMode);

    QChar currentChar = toPlainText().at(cursor.position() - 1);

    // Just to next non-space char if current char is space.
    if (currentChar.isSpace()) {
        while (cursor.position() < toPlainText().length() && currentChar.isSpace()) {
            cursor.movePosition(QTextCursor::NextCharacter, moveMode);
            currentChar = toPlainText().at(cursor.position() - 1);
        }
    }
    // Just to next word-separator char.
    else {
        while (cursor.position() < toPlainText().length() && !atWordSeparator(cursor.position())) {
            cursor.movePosition(QTextCursor::NextCharacter, moveMode);
        }
    }

    return cursor.position();
}

int TextEditor::getPrevWordPosition(QTextCursor cursor, QTextCursor::MoveMode moveMode)
{
    if (toPlainText().isEmpty()) {
        return 0;
    }

    // Move prev char first.
    cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);

    QChar currentChar = toPlainText().at(cursor.position());

    // Just to next non-space char if current char is space.
    if (currentChar.isSpace()) {
        while (cursor.position() > 0 && currentChar.isSpace()) {
            cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
            currentChar = toPlainText().at(cursor.position());
        }
    }
    // Just to next word-separator char.
    else {
        while (cursor.position() > 0 && !atWordSeparator(cursor.position())) {
            cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
        }
    }

    return cursor.position();
}

bool TextEditor::atWordSeparator(int position)
{
    return m_wordSepartors.contains(QString(toPlainText().at(position)));
}

void TextEditor::showCursorBlink()
{
    // the default value on X11 is 1000 milliseconds.
    QApplication::setCursorFlashTime(1000);
}

void TextEditor::hideCursorBlink()
{
    QApplication::setCursorFlashTime(0);
}

void TextEditor::completionWord(QString word)
{
    QString wordAtCursor = getWordAtCursor();
    QTextCursor cursor = textCursor();

    QString completionString = word.remove(0, wordAtCursor.size());
    if (completionString.size() > 0) {
        cursor = textCursor();
        cursor.insertText(completionString);

        setTextCursor(cursor);
    }
}

bool TextEditor::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        m_mouseClickPos = QCursor::pos();

        emit click();
    }

    return false;
}

void TextEditor::adjustScrollbarMargins()
{
    if (!isVisible()) {
        return;
    }

    QEvent event(QEvent::LayoutRequest);
    QApplication::sendEvent(this, &event);

    if (!verticalScrollBar()->visibleRegion().isEmpty()) {
        setViewportMargins(0, 0, -verticalScrollBar()->sizeHint().width(), 0);
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
}

void TextEditor::dragEnterEvent(QDragEnterEvent *event)
{
    QPlainTextEdit::dragEnterEvent(event);
    qobject_cast<Window *>(this->window())->requestDragEnterEvent(event);
}

void TextEditor::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *data = event->mimeData();

    if (data->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QPlainTextEdit::dragMoveEvent(event);
    }
}

void TextEditor::dropEvent(QDropEvent *event)
{
    const QMimeData *data = event->mimeData();

    if (data->hasUrls() && data->urls().first().isLocalFile()) {
        qobject_cast<Window *>(this->window())->requestDropEvent(event);
    } else if (data->hasText()) {
        QPlainTextEdit::dropEvent(event);
    }
}
