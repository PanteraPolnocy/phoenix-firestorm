/**
 * @file lltexteditor.cpp
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// Text editor widget to let users enter a a multi-line ASCII document.

#include "linden_common.h"

#define LLTEXTEDITOR_CPP
#include "lltexteditor.h"

#include "llfontfreetype.h" // for LLFontFreetype::FIRST_CHAR
#include "llfontgl.h"
#include "llgl.h"           // LLGLSUIDefault()
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llrect.h"
#include "llfocusmgr.h"
#include "lltimer.h"
#include "llmath.h"

#include "llclipboard.h"
#include "llemojihelper.h"
#include "llscrollbar.h"
#include "llstl.h"
#include "llstring.h"
#include "llkeyboard.h"
#include "llkeywords.h"
#include "llundo.h"
#include "llviewborder.h"
#include "llcontrol.h"
#include "llwindow.h"
#include "lltextparser.h"
#include "llscrollcontainer.h"
#include "llspellcheck.h"
#include "llpanel.h"
#include "llurlregistry.h"
#include "lltooltip.h"
#include "llmenugl.h"
#include "llchatmentionhelper.h"

#include <queue>
#include "llcombobox.h"

//
// Globals
//
static LLDefaultChildRegistry::Register<LLTextEditor> r("simple_text_editor");

// Compiler optimization, generate extern template
template class LLTextEditor* LLView::getChild<class LLTextEditor>(
    std::string_view name, bool recurse) const;

//
// Constants
//
const S32   SPACES_PER_TAB = 4;
const F32   SPELLCHECK_DELAY = 0.5f;    // delay between the last keypress and spell checking the word the cursor is on

///////////////////////////////////////////////////////////////////

class LLTextEditor::TextCmdInsert : public LLTextBase::TextCmd
{
public:
    TextCmdInsert(S32 pos, bool group_with_next, const LLWString &ws, LLTextSegmentPtr segment)
        : TextCmd(pos, group_with_next, segment), mWString(ws)
    {
    }
    virtual ~TextCmdInsert() {}
    virtual bool execute( LLTextBase* editor, S32* delta )
    {
        *delta = insert(editor, getPosition(), mWString );
        LLWStringUtil::truncate(mWString, *delta);
        //mWString = wstring_truncate(mWString, *delta);
        return (*delta != 0);
    }
    virtual S32 undo( LLTextBase* editor )
    {
        remove(editor, getPosition(), static_cast<S32>(mWString.length()));
        return getPosition();
    }
    virtual S32 redo( LLTextBase* editor )
    {
        insert(editor, getPosition(), mWString);
        return getPosition() + static_cast<S32>(mWString.length());
    }

private:
    LLWString mWString;
};

///////////////////////////////////////////////////////////////////
class LLTextEditor::TextCmdAddChar : public LLTextBase::TextCmd
{
public:
    TextCmdAddChar( S32 pos, bool group_with_next, llwchar wc, LLTextSegmentPtr segment)
        : TextCmd(pos, group_with_next, segment), mWString(1, wc), mBlockExtensions(false)
    {
    }
    virtual void blockExtensions()
    {
        mBlockExtensions = true;
    }
    virtual bool canExtend(S32 pos) const
    {
        // cannot extend text with custom segments
        if (!mSegments.empty()) return false;

        return !mBlockExtensions && (pos == getPosition() + (S32)mWString.length());
    }
    virtual bool execute( LLTextBase* editor, S32* delta )
    {
        *delta = insert(editor, getPosition(), mWString);
        LLWStringUtil::truncate(mWString, *delta);
        //mWString = wstring_truncate(mWString, *delta);
        return (*delta != 0);
    }
    virtual bool extendAndExecute( LLTextBase* editor, S32 pos, llwchar wc, S32* delta )
    {
        LLWString ws;
        ws += wc;

        *delta = insert(editor, pos, ws);
        if( *delta > 0 )
        {
            mWString += wc;
        }
        return (*delta != 0);
    }
    virtual S32 undo( LLTextBase* editor )
    {
        remove(editor, getPosition(), static_cast<S32>(mWString.length()));
        return getPosition();
    }
    virtual S32 redo( LLTextBase* editor )
    {
        insert(editor, getPosition(), mWString);
        return getPosition() + static_cast<S32>(mWString.length());
    }

private:
    LLWString   mWString;
    bool        mBlockExtensions;

};

///////////////////////////////////////////////////////////////////

class LLTextEditor::TextCmdOverwriteChar : public LLTextBase::TextCmd
{
public:
    TextCmdOverwriteChar( S32 pos, bool group_with_next, llwchar wc)
        : TextCmd(pos, group_with_next), mChar(wc), mOldChar(0) {}

    virtual bool execute( LLTextBase* editor, S32* delta )
    {
        mOldChar = editor->getWText()[getPosition()];
        overwrite(editor, getPosition(), mChar);
        *delta = 0;
        return true;
    }
    virtual S32 undo( LLTextBase* editor )
    {
        overwrite(editor, getPosition(), mOldChar);
        return getPosition();
    }
    virtual S32 redo( LLTextBase* editor )
    {
        overwrite(editor, getPosition(), mChar);
        return getPosition()+1;
    }

private:
    llwchar     mChar;
    llwchar     mOldChar;
};

///////////////////////////////////////////////////////////////////

class LLTextEditor::TextCmdRemove : public LLTextBase::TextCmd
{
public:
    TextCmdRemove( S32 pos, bool group_with_next, S32 len, segment_vec_t& segments ) :
        TextCmd(pos, group_with_next), mLen(len)
    {
        std::swap(mSegments, segments);
    }
    virtual bool execute( LLTextBase* editor, S32* delta )
    {
        try
        {
            mWString = editor->getWText().substr(getPosition(), mLen);
            *delta = remove(editor, getPosition(), mLen);
        }
        catch (std::out_of_range&)
        {
            return false;
        }
        return (*delta != 0);
    }
    virtual S32 undo( LLTextBase* editor )
    {
        insert(editor, getPosition(), mWString);
        return getPosition() + static_cast<S32>(mWString.length());
    }
    virtual S32 redo( LLTextBase* editor )
    {
        remove(editor, getPosition(), mLen );
        return getPosition();
    }
private:
    LLWString   mWString;
    S32             mLen;
};


///////////////////////////////////////////////////////////////////
LLTextEditor::Params::Params()
:   default_text("default_text"),
    prevalidator("prevalidator"),
    embedded_items("embedded_items", false),
    ignore_tab("ignore_tab", true),
    auto_indent("auto_indent", true),
    default_color("default_color"),
    commit_on_focus_lost("commit_on_focus_lost", false),
    show_context_menu("show_context_menu"),
    show_emoji_helper("show_emoji_helper"),
    enable_tooltip_paste("enable_tooltip_paste"),
    enable_tab_remove("enable_tab_remove", true)    // <FS:Ansariel> FIRE-15591: Optional tab remove
{
    addSynonym(prevalidator, "prevalidate_callback");
    addSynonym(prevalidator, "text_type");
}

LLTextEditor::LLTextEditor(const LLTextEditor::Params& p) :
    LLTextBase(p),
    mAutoreplaceCallback(),
    mBaseDocIsPristine(true),
    mPristineCmd( NULL ),
    mLastCmd( NULL ),
    mDefaultColor( p.default_color() ),
    mAutoIndent(p.auto_indent),
    mParseOnTheFly(false),
    mCommitOnFocusLost( p.commit_on_focus_lost),
    mAllowEmbeddedItems( p.embedded_items ),
    mMouseDownX(0),
    mMouseDownY(0),
    mTabsToNextField(p.ignore_tab),
    mPrevalidator(p.prevalidator()),
    mShowContextMenu(p.show_context_menu),
    mShowEmojiHelper(p.show_emoji_helper),
    mShowChatMentionPicker(false),
    mEnableTooltipPaste(p.enable_tooltip_paste),
    mPassDelete(false),
    mKeepSelectionOnReturn(false),
    mEnableTabRemove(p.enable_tab_remove)   // <FS:Ansariel> FIRE-15591: Optional tab remove
{
    mSourceID.generate();

    //FIXME: use image?
    LLViewBorder::Params params;
    params.name = "text ed border";
    params.rect = getLocalRect();
    params.bevel_style = LLViewBorder::BEVEL_IN;
    params.border_thickness = 1;
    params.visible = p.border_visible;
    mBorder = LLUICtrlFactory::create<LLViewBorder> (params);
    addChild( mBorder );
    setText(p.default_text());

    mParseOnTheFly = true;
}

void LLTextEditor::initFromParams( const LLTextEditor::Params& p)
{
    LLTextBase::initFromParams(p);

    // HACK:  text editors always need to be enabled so that we can scroll
    LLView::setEnabled(true);

    if (p.commit_on_focus_lost.isProvided())
    {
        mCommitOnFocusLost = p.commit_on_focus_lost;
    }

    updateAllowingLanguageInput();
}

LLTextEditor::~LLTextEditor()
{
    gFocusMgr.releaseFocusIfNeeded( this ); // calls onCommit() while LLTextEditor still valid

    // Scrollbar is deleted by LLView
    std::for_each(mUndoStack.begin(), mUndoStack.end(), DeletePointer());
    mUndoStack.clear();
    // Mark the menu as dead or its retained in memory till shutdown.
    LLContextMenu* menu = static_cast<LLContextMenu*>(mContextMenuHandle.get());
    if(menu)
    {
        menu->die();
        mContextMenuHandle.markDead();
    }
}

////////////////////////////////////////////////////////////
// LLTextEditor
// Public methods

void LLTextEditor::setText(const LLStringExplicit &utf8str, const LLStyle::Params& input_params)
{
    // validate incoming text if necessary
    if (mPrevalidator)
    {
        if (!mPrevalidator.validate(utf8str))
        {
            LLUI::getInstance()->reportBadKeystroke();
            mPrevalidator.showLastErrorUsingTimeout();

            // not valid text, nothing to do
            return;
        }
    }

    blockUndo();
    deselect();

    mParseOnTheFly = false;
    LLTextBase::setText(utf8str, input_params);
    mParseOnTheFly = true;

    resetDirty();
}

// <AS:Chanayane> Preview button
void LLTextEditor::reparseText(const LLStringExplicit &utf8str, const LLStyle::Params& input_params)
{
    mParseOnTheFly = false;
    LLTextBase::setText(utf8str, input_params);
    mParseOnTheFly = true;
}

void LLTextEditor::reparseValue(const LLSD& value)
{
    reparseText(value.asString());
}
// </AS:Chanayane>

// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2013-12-30 (Catznip-3.6)
std::string LLTextEditor::getSelectionString() const
{
    S32 idxSel = 0, lenSel = 0;
    getSelectionRange(&idxSel, &lenSel);
    return (lenSel > 0) ? wstring_to_utf8str(getWText().substr(idxSel, lenSel)) : LLStringUtil::null;
}
// [/SL:KB]

//void LLTextEditor::selectNext(const std::string& search_text_in, bool case_insensitive, bool wrap)
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
void LLTextEditor::selectNext(const std::string& search_text_in, bool case_insensitive, bool wrap, bool search_up)
// [/SL:KB]
{
    if (search_text_in.empty())
    {
        return;
    }

    LLWString text = getWText();
    LLWString search_text = utf8str_to_wstring(search_text_in);
    if (case_insensitive)
    {
        LLWStringUtil::toLower(text);
        LLWStringUtil::toLower(search_text);
    }

    if (mIsSelecting)
    {
        LLWString selected_text = text.substr(mSelectionEnd, mSelectionStart - mSelectionEnd);

        if (selected_text == search_text)
        {
//          // We already have this word selected, we are searching for the next.
//          setCursorPos(mCursorPos + static_cast<S32>(search_text.size()));
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
            if (search_up)
            {
                // We already have this word selected, we are searching for the previous.
                setCursorPos(llmax(0, mCursorPos - 1));
            }
            else
            {
                // We already have this word selected, we are searching for the next.
                setCursorPos(mCursorPos + static_cast<S32>(search_text.size()));
            }
// [/SL:KB]
        }
    }

//  S32 loc = static_cast<S32>(text.find(search_text,mCursorPos));
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
    S32 loc = (search_up) ? static_cast<S32>(text.rfind(search_text, llmax(0, mCursorPos - (S32)search_text.size()))) : static_cast<S32>(text.find(search_text,mCursorPos));
// [/SL:KB]

    // If Maybe we wrapped, search again
    if (wrap && (-1 == loc))
    {
//      loc = static_cast<S32>(text.find(search_text));
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
        loc = (search_up) ? static_cast<S32>(text.rfind(search_text)) : static_cast<S32>(text.find(search_text));
// [/SL:KB]
    }

    // If still -1, then search_text just isn't found.
    if (-1 == loc)
    {
        mIsSelecting = false;
        mSelectionEnd = 0;
        mSelectionStart = 0;
        return;
    }

    setCursorPos(loc);
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-11-05 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
    if (mReadOnly)
    {
        updateScrollFromCursor();
    }
// [/SL:KB]

    mIsSelecting = true;
    mSelectionEnd = mCursorPos;
    mSelectionStart = llmin((S32)getLength(), (S32)(mCursorPos + search_text.size()));
}

//bool LLTextEditor::replaceText(const std::string& search_text_in, const std::string& replace_text,
//                             bool case_insensitive, bool wrap)
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
bool LLTextEditor::replaceText(const std::string& search_text_in, const std::string& replace_text,
                               bool case_insensitive, bool wrap, bool search_up)
// [/SL:KB]
{
    bool replaced = false;

    if (search_text_in.empty())
    {
        return replaced;
    }

    LLWString search_text = utf8str_to_wstring(search_text_in);
    if (mIsSelecting)
    {
        LLWString text = getWText();
        LLWString selected_text = text.substr(mSelectionEnd, mSelectionStart - mSelectionEnd);

        if (case_insensitive)
        {
            LLWStringUtil::toLower(selected_text);
            LLWStringUtil::toLower(search_text);
        }

        if (selected_text == search_text)
        {
            insertText(replace_text);
            replaced = true;
        }
    }

//  selectNext(search_text_in, case_insensitive, wrap);
// [SL:KB] - Patch: UI-FloaterSearchReplace | Checked: 2010-10-29 (Catznip-2.3.0a) | Added: Catznip-2.3.0a
    selectNext(search_text_in, case_insensitive, wrap, search_up);
// [/SL:KB]
    return replaced;
}

void LLTextEditor::replaceTextAll(const std::string& search_text, const std::string& replace_text, bool case_insensitive)
{
    startOfDoc();
    selectNext(search_text, case_insensitive, false);

    bool replaced = true;
    while ( replaced )
    {
        replaced = replaceText(search_text,replace_text, case_insensitive, false);
    }
}

S32 LLTextEditor::prevWordPos(S32 cursorPos) const
{
    LLWString wtext(getWText());
    while( (cursorPos > 0) && (wtext[cursorPos-1] == ' ') )
    {
        cursorPos--;
    }
    while( (cursorPos > 0) && LLWStringUtil::isPartOfWord( wtext[cursorPos-1] ) )
    {
        cursorPos--;
    }
    return cursorPos;
}

S32 LLTextEditor::nextWordPos(S32 cursorPos) const
{
    LLWString wtext(getWText());
    while( (cursorPos < getLength()) && LLWStringUtil::isPartOfWord( wtext[cursorPos] ) )
    {
        cursorPos++;
    }
    while( (cursorPos < getLength()) && (wtext[cursorPos] == ' ') )
    {
        cursorPos++;
    }
    return cursorPos;
}

const LLTextSegmentPtr  LLTextEditor::getPreviousSegment() const
{
    static LLPointer<LLIndexSegment> index_segment = new LLIndexSegment;

    index_segment->setStart(mCursorPos);
    index_segment->setEnd(mCursorPos);

    // find segment index at character to left of cursor (or rightmost edge of selection)
    segment_set_t::const_iterator it = mSegments.lower_bound(index_segment);

    if (it != mSegments.end())
    {
        return *it;
    }
    else
    {
        return LLTextSegmentPtr();
    }
}

void LLTextEditor::getSelectedSegments(LLTextEditor::segment_vec_t& segments) const
{
    S32 left = hasSelection() ? llmin(mSelectionStart, mSelectionEnd) : mCursorPos;
    S32 right = hasSelection() ? llmax(mSelectionStart, mSelectionEnd) : mCursorPos;

    return getSegmentsInRange(segments, left, right, true);
}

void LLTextEditor::getSegmentsInRange(LLTextEditor::segment_vec_t& segments_out, S32 start, S32 end, bool include_partial) const
{
    segment_set_t::const_iterator first_it = getSegIterContaining(start);
    segment_set_t::const_iterator end_it = getSegIterContaining(end - 1);
    if (end_it != mSegments.end()) ++end_it;

    // <FS:ND> FIRE-3278; end_it can point before first_it (std::distance( first_it, mSegments.end() ) < std::distance( end_it, mSegments.end() ))
    // In that case the loop below does not correctly terminate. Checking for it != mSegments.end() too, that will avoid that error

    // for (segment_set_t::const_iterator it = first_it; it != end_it; ++it)
    for (segment_set_t::const_iterator it = first_it; it != mSegments.end() && it != end_it; ++it)
    // </FS:ND>
    {
        LLTextSegmentPtr segment = *it;
        if (include_partial
            ||  (segment->getStart() >= start
                && segment->getEnd() <= end))
        {
            segments_out.push_back(segment);
        }
    }
}

void LLTextEditor::setShowEmojiHelper(bool show)
{
    if (!mShowEmojiHelper)
    {
        LLEmojiHelper::instance().hideHelper(this);
    }

    mShowEmojiHelper = show;
}

bool LLTextEditor::selectionContainsLineBreaks()
{
    if (hasSelection())
    {
        S32 left = llmin(mSelectionStart, mSelectionEnd);
        S32 right = left + llabs(mSelectionStart - mSelectionEnd);

        LLWString wtext = getWText();
        for( S32 i = left; i < right; i++ )
        {
            if (wtext[i] == '\n')
            {
                return true;
            }
        }
    }
    return false;
}


S32 LLTextEditor::indentLine( S32 pos, S32 spaces )
{
    // Assumes that pos is at the start of the line
    // spaces may be positive (indent) or negative (unindent).
    // Returns the actual number of characters added or removed.

    llassert(pos >= 0);
    llassert(pos <= getLength() );

    S32 delta_spaces = 0;

    if (spaces >= 0)
    {
        // Indent
        for(S32 i=0; i < spaces; i++)
        {
            delta_spaces += addChar(pos, ' ');
        }
    }
    else
    {
        // Unindent
        for(S32 i=0; i < -spaces; i++)
        {
            LLWString wtext = getWText();
            if (wtext[pos] == ' ')
            {
                delta_spaces += remove( pos, 1, false );
            }
        }
    }

    return delta_spaces;
}

void LLTextEditor::indentSelectedLines( S32 spaces )
{
    if( hasSelection() )
    {
        LLWString text = getWText();
        S32 left = llmin( mSelectionStart, mSelectionEnd );
        S32 right = left + llabs( mSelectionStart - mSelectionEnd );
        bool cursor_on_right = (mSelectionEnd > mSelectionStart);
        S32 cur = left;

        // Expand left to start of line
        while( (cur > 0) && (text[cur] != '\n') )
        {
            cur--;
        }
        left = cur;
        if( cur > 0 )
        {
            left++;
        }

        // Expand right to end of line
        if( text[right - 1] == '\n' )
        {
            right--;
        }
        else
        {
            while( (text[right] != '\n') && (right <= getLength() ) )
            {
                right++;
            }
        }

        // Disabling parsing on the fly to avoid updating text segments
        // until all indentation commands are executed.
        mParseOnTheFly = false;

        // Find each start-of-line and indent it
        do
        {
            if( text[cur] == '\n' )
            {
                cur++;
            }

            S32 delta_spaces = indentLine( cur, spaces );
            if( delta_spaces > 0 )
            {
                cur += delta_spaces;
            }
            right += delta_spaces;

            text = getWText();

            // Find the next new line
            while( (cur < right) && (text[cur] != '\n') )
            {
                cur++;
            }
        }
        while( cur < right );

        mParseOnTheFly = true;

        if( (right < getLength()) && (text[right] == '\n') )
        {
            right++;
        }

        // Set the selection and cursor
        if( cursor_on_right )
        {
            mSelectionStart = left;
            mSelectionEnd = right;
        }
        else
        {
            mSelectionStart = right;
            mSelectionEnd = left;
        }
        setCursorPos(mSelectionEnd);
    }
}

//virtual
bool LLTextEditor::canSelectAll() const
{
    return true;
}

// virtual
void LLTextEditor::selectAll()
{
    mSelectionStart = getLength();
    mSelectionEnd = 0;
    setCursorPos(mSelectionEnd);
    // <FS:AW> Linux primary "clipboard" tainted by auto-selection
    //updatePrimary();
}

void LLTextEditor::selectByCursorPosition(S32 prev_cursor_pos, S32 next_cursor_pos)
{
    setCursorPos(prev_cursor_pos);
    startSelection();
    setCursorPos(next_cursor_pos);
    endSelection();
}

void LLTextEditor::insertEmoji(llwchar emoji)
{
    static LLUICachedControl<bool> useBWEmojis( "FSUseBWEmojis", false); // <FS:Beq/> Add B&W emoji font support
    LL_DEBUGS("Emoji") << "LLTextEditor::insertEmoji(" << wchar_utf8_preview(emoji) << ")" << LL_ENDL;  // <FS:Beq/> reduce Emoji log spam
    auto styleParams = LLStyle::Params();
    styleParams.font = LLFontGL::getFontEmojiLarge(useBWEmojis); // <FS:Beq/> Add B&W emoji font support
    auto segment = new LLEmojiTextSegment(new LLStyle(styleParams), mCursorPos, mCursorPos + 1, *this);
    insert(mCursorPos, LLWString(1, emoji), false, segment);
    setCursorPos(mCursorPos + 1);
}

void LLTextEditor::handleEmojiCommit(llwchar emoji)
{
    S32 shortCodePos;
    if (LLEmojiHelper::isCursorInEmojiCode(getWText(), mCursorPos, &shortCodePos))
    {
        remove(shortCodePos, mCursorPos - shortCodePos, true);
        setCursorPos(shortCodePos);

        insertEmoji(emoji);
    }
}

void LLTextEditor::handleMentionCommit(std::string name_url)
{
    S32 mention_start_pos;
    if (LLChatMentionHelper::instance().isCursorInNameMention(getWText(), mCursorPos, &mention_start_pos))
    {
        remove(mention_start_pos, mCursorPos - mention_start_pos, true);
        insert(mention_start_pos, utf8str_to_wstring(name_url), false, LLTextSegmentPtr());

        std::string new_text(wstring_to_utf8str(getConvertedText()));
        clear();
        appendTextImpl(new_text, LLStyle::Params(), true);

        segment_set_t::const_iterator it = getSegIterContaining(mention_start_pos);
        if (it != mSegments.end())
        {
            setCursorPos((*it)->getEnd() + 1);
        }
        else
        {
            setCursorPos(mention_start_pos);
        }
    }
}

bool LLTextEditor::handleMouseDown(S32 x, S32 y, MASK mask)
{
    bool    handled = false;

    // set focus first, in case click callbacks want to change it
    // RN: do we really need to have a tab stop?
    if (hasTabStop())
    {
        setFocus( true );
    }

    // Let scrollbar have first dibs
    handled = LLTextBase::handleMouseDown(x, y, mask);

    if( !handled )
    {
        if (!(mask & MASK_SHIFT))
        {
            deselect();
        }

        bool start_select = true;
        if( start_select )
        {
            // If we're not scrolling (handled by child), then we're selecting
            if (mask & MASK_SHIFT)
            {
                S32 old_cursor_pos = mCursorPos;
                setCursorAtLocalPos( x, y, true );

                if (hasSelection())
                {
                    mSelectionEnd = mCursorPos;
                }
                else
                {
                    mSelectionStart = old_cursor_pos;
                    mSelectionEnd = mCursorPos;
                }
                // assume we're starting a drag select
                mIsSelecting = true;
            }
            else
            {
                setCursorAtLocalPos( x, y, true );
                startSelection();
            }
        }

        handled = true;
    }

    // Delay cursor flashing
    resetCursorBlink();

    if (handled && !gFocusMgr.getMouseCapture())
    {
        gFocusMgr.setMouseCapture( this );
    }
    return handled;
}

bool LLTextEditor::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (hasTabStop())
    {
        setFocus(true);
    }
// [SL:KB] - Patch: UI-Notecards | Checked: 2010-09-12 (Catznip-2.1.2d) | Added: Catznip-2.1.2d
    setCursorAtLocalPos(x, y, false);
// [/SL:KB]

    bool show_menu = false;

    // Prefer editor menu if it has selection. See EXT-6806.
    if (hasSelection())
    {
        S32 click_pos = getDocIndexFromLocalCoord(x, y, false);
        if (click_pos > mSelectionStart && click_pos < mSelectionEnd)
        {
            show_menu = true;
        }
    }

    // Let segments handle the click, if nothing does, show editor menu
    if (!show_menu && !LLTextBase::handleRightMouseDown(x, y, mask))
    {
        show_menu = true;
    }

    if (show_menu && getShowContextMenu())
    {
        showContextMenu(x, y);
    }

    return true;
}



bool LLTextEditor::handleMiddleMouseDown(S32 x, S32 y, MASK mask)
{
    if (hasTabStop())
    {
        setFocus(true);
    }

    if (!LLTextBase::handleMouseDown(x, y, mask))
    {
        if( canPastePrimary() )
        {
            setCursorAtLocalPos( x, y, true );
            // does not rely on focus being set
            pastePrimary();
        }
    }
    return true;
}


bool LLTextEditor::handleHover(S32 x, S32 y, MASK mask)
{
    bool handled = false;

    if(hasMouseCapture() )
    {
        if( mIsSelecting )
        {
            if(mScroller)
            {
                mScroller->autoScroll(x, y);
            }
            S32 clamped_x = llclamp(x, mVisibleTextRect.mLeft, mVisibleTextRect.mRight);
            S32 clamped_y = llclamp(y, mVisibleTextRect.mBottom, mVisibleTextRect.mTop);
            setCursorAtLocalPos( clamped_x, clamped_y, true );
            mSelectionEnd = mCursorPos;
        }
        LL_DEBUGS("UserInput") << "hover handled by " << getName() << " (active)" << LL_ENDL;
        getWindow()->setCursor(UI_CURSOR_IBEAM);
        handled = true;
    }

    if( !handled )
    {
        // Pass to children
        handled = LLTextBase::handleHover(x, y, mask);
    }

    if( handled )
    {
        // Delay cursor flashing
        resetCursorBlink();
    }

    if( !handled )
    {
        getWindow()->setCursor(UI_CURSOR_IBEAM);
        handled = true;
    }

    return handled;
}


bool LLTextEditor::handleMouseUp(S32 x, S32 y, MASK mask)
{
    bool    handled = false;

    // if I'm not currently selecting text
    //if (!(mIsSelecting && hasMouseCapture()))
// [SL:KB] - Patch: Control-TextEditor | Checked: 2014-02-04 (Catznip)
    if (!(hasSelection() && mIsSelecting && hasMouseCapture()))
// [/SL:KB]
    {
        // let text segments handle mouse event
        handled = LLTextBase::handleMouseUp(x, y, mask);
    }

    if( !handled )
    {
        if( mIsSelecting )
        {
            if(mScroller)
            {
                mScroller->autoScroll(x, y);
            }
            S32 clamped_x = llclamp(x, mVisibleTextRect.mLeft, mVisibleTextRect.mRight);
            S32 clamped_y = llclamp(y, mVisibleTextRect.mBottom, mVisibleTextRect.mTop);
            setCursorAtLocalPos( clamped_x, clamped_y, true );
            endSelection();
        }

        // take selection to 'primary' clipboard
        updatePrimary();

        handled = true;
    }

    // Delay cursor flashing
    resetCursorBlink();

    if( hasMouseCapture()  )
    {
        gFocusMgr.setMouseCapture( NULL );

        handled = true;
    }

    return handled;
}


bool LLTextEditor::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    bool    handled = false;

    // let scrollbar and text segments have first dibs
    handled = LLTextBase::handleDoubleClick(x, y, mask);

    if( !handled )
    {
        setCursorAtLocalPos( x, y, false );
        deselect();

        LLWString text = getWText();

        if( LLWStringUtil::isPartOfWord( text[mCursorPos] ) )
        {
            // Select word the cursor is over
            while ((mCursorPos > 0) && LLWStringUtil::isPartOfWord(text[mCursorPos-1]))
            {
                if (!setCursorPos(mCursorPos - 1)) break;
            }
            startSelection();

            while ((mCursorPos < (S32)text.length()) && LLWStringUtil::isPartOfWord( text[mCursorPos] ) )
            {
                if (!setCursorPos(mCursorPos + 1)) break;
            }

            mSelectionEnd = mCursorPos;
        }
        else if ((mCursorPos < (S32)text.length()) && !iswspace( text[mCursorPos]) )
        {
            // Select the character the cursor is over
            startSelection();
            setCursorPos(mCursorPos + 1);
            mSelectionEnd = mCursorPos;
        }

        // We don't want handleMouseUp() to "finish" the selection (and thereby
        // set mSelectionEnd to where the mouse is), so we finish the selection here.
        mIsSelecting = false;

        // delay cursor flashing
        resetCursorBlink();

        // take selection to 'primary' clipboard
        // <FS:AW> Linux primary "clipboard" tainted by auto-selection
        //updatePrimary();

        handled = true;
    }

    return handled;
}


//----------------------------------------------------------------------------
// Returns change in number of characters in mText

S32 LLTextEditor::execute( TextCmd* cmd )
{
    if (!mReadOnly && mShowEmojiHelper)
    {
        // Any change to our contents should always hide the helper
        LLEmojiHelper::instance().hideHelper(this);
    }

    S32 delta = 0;
    if( cmd->execute(this, &delta) )
    {
        // Delete top of undo stack
        undo_stack_t::iterator enditer = std::find(mUndoStack.begin(), mUndoStack.end(), mLastCmd);
        std::for_each(mUndoStack.begin(), enditer, DeletePointer());
        mUndoStack.erase(mUndoStack.begin(), enditer);
        // Push the new command is now on the top (front) of the undo stack.
        mUndoStack.push_front(cmd);
        mLastCmd = cmd;

        bool need_to_rollback = mPrevalidator && !mPrevalidator.validate(getViewModel()->getDisplay());
        if (need_to_rollback)
        {
            LLUI::getInstance()->reportBadKeystroke();
            mPrevalidator.showLastErrorUsingTimeout();

            // get rid of this last command and clean up undo stack
            undo();

            // remove any evidence of this command from redo history
            mUndoStack.pop_front();
            delete cmd;

            // failure, nothing changed
            delta = 0;
        }
    }
    else
    {
        // Operation failed, so don't put it on the undo stack.
        delete cmd;
    }

    return delta;
}

S32 LLTextEditor::insert(S32 pos, const LLWString &wstr, bool group_with_next_op, LLTextSegmentPtr segment)
{
    return execute( new TextCmdInsert( pos, group_with_next_op, wstr, segment ) );
}

S32 LLTextEditor::remove(S32 pos, S32 length, bool group_with_next_op)
{
    S32 end_pos = getEditableIndex(pos + length, true);
    bool removedChar = false;

    segment_vec_t segments_to_remove;
    // store text segments
    getSegmentsInRange(segments_to_remove, pos, pos + length, false);

    if (pos <= end_pos)
    {
        removedChar = execute( new TextCmdRemove( pos, group_with_next_op, end_pos - pos, segments_to_remove ) );
    }

    return removedChar;
}

S32 LLTextEditor::overwriteChar(S32 pos, llwchar wc)
{
    if ((S32)getLength() == pos)
    {
        return addChar(pos, wc);
    }
    else
    {
        return execute(new TextCmdOverwriteChar(pos, false, wc));
    }
}

// Remove a single character from the text.  Tries to remove
// a pseudo-tab (up to for spaces in a row)
void LLTextEditor::removeCharOrTab()
{
    if (!getEnabled())
    {
        return;
    }

    if (mCursorPos > 0)
    {
        S32 chars_to_remove = 1;

        LLWString text = getWText();
        // <FS:Ansariel> FIRE-15591: Optional tab remove
        //if (text[mCursorPos - 1] == ' ')
        if (mEnableTabRemove && text[mCursorPos - 1] == ' ')
        // </FS:Ansariel>
        {
            // Try to remove a "tab"
            S32 offset = getLineOffsetFromDocIndex(mCursorPos);
            if (offset > 0)
            {
                chars_to_remove = offset % SPACES_PER_TAB;
                if (chars_to_remove == 0)
                {
                    chars_to_remove = SPACES_PER_TAB;
                }

                for (S32 i = 0; i < chars_to_remove; i++)
                {
                    if (text[mCursorPos - i - 1] != ' ')
                    {
                        // Fewer than a full tab's worth of spaces, so
                        // just delete a single character.
                        chars_to_remove = 1;
                        break;
                    }
                }
            }
        }

        for (S32 i = 0; i < chars_to_remove; i++)
        {
            setCursorPos(mCursorPos - 1);
            remove(mCursorPos, 1, false);
        }

        tryToShowEmojiHelper();
        tryToShowMentionHelper();
    }
    else
    {
        LLUI::getInstance()->reportBadKeystroke();
    }
}

// Remove a single character from the text
S32 LLTextEditor::removeChar(S32 pos)
{
    return remove(pos, 1, false);
}

void LLTextEditor::removeChar()
{
    if (!getEnabled())
    {
        return;
    }

    if (mCursorPos > 0)
    {
        setCursorPos(mCursorPos - 1);
        removeChar(mCursorPos);
        tryToShowEmojiHelper();
        tryToShowMentionHelper();
    }
    else
    {
        LLUI::getInstance()->reportBadKeystroke();
    }
}

// <FS> Ctrl-Backspace remove word
// Remove a word (set of characters up to next space/punctuation) from the text
void LLTextEditor::removeWord(bool prev)
{
    const U32 pos(mCursorPos);
    if (prev ? pos > 0 : static_cast<S32>(pos) < getLength())
    {
        U32 new_pos(prev ? prevWordPos(pos) : nextWordPos(pos));
        if (new_pos == pos) // Other character we don't jump over
            new_pos = prev ? prevWordPos(new_pos-1) : nextWordPos(new_pos+1);

        const U32 diff(labs(static_cast<S32>(pos) - static_cast<S32>(new_pos)));
        if (prev)
        {
            remove(new_pos, diff, false);
            setCursorPos(new_pos);
        }
        else
        {
            remove(pos, diff, false);
        }
    }
    else
    {
        LLUI::getInstance()->reportBadKeystroke();
    }
}
// </FS>

// Add a single character to the text
S32 LLTextEditor::addChar(S32 pos, llwchar wc)
{
    if ((wstring_utf8_length(getWText()) + wchar_utf8_length(wc)) > mMaxTextByteLength)
    {
        LLUI::getInstance()->reportBadKeystroke();
        return 0;
    }

    if (mLastCmd && mLastCmd->canExtend(pos))
    {
        if (mPrevalidator)
        {
            // get a copy of current text contents
            LLWString test_string(getViewModel()->getDisplay());

            // modify text contents as if this addChar succeeded
            llassert(pos <= (S32)test_string.size());
            test_string.insert(pos, 1, wc);
            if (!mPrevalidator.validate(test_string))
            {
                LLUI::getInstance()->reportBadKeystroke();
                mPrevalidator.showLastErrorUsingTimeout();
                return 0;
            }
        }

        S32 delta = 0;
        mLastCmd->extendAndExecute(this, pos, wc, &delta);

        return delta;
    }

    return execute(new TextCmdAddChar(pos, false, wc, LLTextSegmentPtr()));
}

void LLTextEditor::addChar(llwchar wc)
{
    if (!getEnabled())
    {
        return;
    }

    if (hasSelection())
    {
        deleteSelection(true);
    }
    else if (LL_KIM_OVERWRITE == gKeyboard->getInsertMode())
    {
        removeChar(mCursorPos);
    }

    setCursorPos(mCursorPos + addChar( mCursorPos, wc ));
    tryToShowEmojiHelper();
    tryToShowMentionHelper();

    if (!mReadOnly && mAutoreplaceCallback != NULL)
    {
        // autoreplace the text, if necessary
        S32 replacement_start;
        S32 replacement_length;
        LLWString replacement_string;
        S32 new_cursor_pos = mCursorPos;
        mAutoreplaceCallback(replacement_start, replacement_length, replacement_string, new_cursor_pos, getWText());

        if (replacement_length > 0 || !replacement_string.empty())
        {
            remove(replacement_start, replacement_length, true);
            insert(replacement_start, replacement_string, false, LLTextSegmentPtr());
            setCursorPos(new_cursor_pos);
        }
    }
}

void LLTextEditor::showEmojiHelper()
{
    if (mReadOnly || !mShowEmojiHelper)
        return;

    const LLRect cursorRect(getLocalRectFromDocIndex(mCursorPos));
    auto cb = [this](llwchar emoji) { insertEmoji(emoji); };
    LLEmojiHelper::instance().showHelper(this, cursorRect.mLeft, cursorRect.mTop, LLStringUtil::null, cb);
}

void LLTextEditor::hideEmojiHelper()
{
    if (mShowEmojiHelper)
    {
        LLEmojiHelper::instance().hideHelper(this);
    }
}

void LLTextEditor::tryToShowEmojiHelper()
{
    if (mReadOnly || !mShowEmojiHelper)
        return;

    S32 shortCodePos;
    LLWString wtext(getWText());
    if (LLEmojiHelper::isCursorInEmojiCode(wtext, mCursorPos, &shortCodePos))
    {
        const LLRect cursorRect(getLocalRectFromDocIndex(shortCodePos));
        const LLWString wpart(wtext.substr(shortCodePos, mCursorPos - shortCodePos));
        const std::string part(wstring_to_utf8str(wpart));
        auto cb = [this](llwchar emoji) { handleEmojiCommit(emoji); };
        LLEmojiHelper::instance().showHelper(this, cursorRect.mLeft, cursorRect.mTop, part, cb);
    }
    else
    {
        LLEmojiHelper::instance().hideHelper();
    }
}

void LLTextEditor::tryToShowMentionHelper()
{
    if (mReadOnly || !mShowChatMentionPicker)
        return;

    S32 mention_start_pos;
    LLWString text(getWText());
    if (LLChatMentionHelper::instance().isCursorInNameMention(text, mCursorPos, &mention_start_pos))
    {
        const LLRect cursor_rect(getLocalRectFromDocIndex(mention_start_pos));
        std::string name_part(wstring_to_utf8str(text.substr(mention_start_pos, mCursorPos - mention_start_pos)));
        name_part.erase(0, 1);
        auto cb = [this](std::string name_url)
        {
            handleMentionCommit(name_url);
        };
        LLChatMentionHelper::instance().showHelper(this, cursor_rect.mLeft, cursor_rect.mTop, name_part, cb);
    }
    else
    {
        LLChatMentionHelper::instance().hideHelper();
    }
}


void LLTextEditor::addLineBreakChar(bool group_together)
{
    if( !getEnabled() )
    {
        return;
    }
    if( hasSelection() )
    {
        deleteSelection(true);
    }
    else if (LL_KIM_OVERWRITE == gKeyboard->getInsertMode())
    {
        removeChar(mCursorPos);
    }

    LLStyleConstSP sp(new LLStyle(LLStyle::Params()));
    LLTextSegmentPtr segment = new LLLineBreakTextSegment(sp, mCursorPos);

    S32 pos = execute(new TextCmdAddChar(mCursorPos, group_together, '\n', segment));

    setCursorPos(mCursorPos + pos);
}


bool LLTextEditor::handleSelectionKey(const KEY key, const MASK mask)
{
    bool handled = false;

    if( mask & MASK_SHIFT )
    {
        handled = true;

        switch( key )
        {
        case KEY_LEFT:
            if( 0 < mCursorPos )
            {
                startSelection();
                setCursorPos(mCursorPos - 1);
                if( mask & MASK_CONTROL )
                {
                    setCursorPos(prevWordPos(mCursorPos));
                }
                mSelectionEnd = mCursorPos;
            }
            break;

        case KEY_RIGHT:
            if( mCursorPos < getLength() )
            {
                startSelection();
                setCursorPos(mCursorPos + 1);
                if( mask & MASK_CONTROL )
                {
                    setCursorPos(nextWordPos(mCursorPos));
                }
                mSelectionEnd = mCursorPos;
            }
            break;

        case KEY_UP:
            startSelection();
            changeLine( -1 );
            mSelectionEnd = mCursorPos;
            break;

        case KEY_PAGE_UP:
            startSelection();
            changePage( -1 );
            mSelectionEnd = mCursorPos;
            break;

        case KEY_HOME:
            startSelection();
            if( mask & MASK_CONTROL )
            {
                setCursorPos(0);
            }
            else
            {
                startOfLine();
            }
            mSelectionEnd = mCursorPos;
            break;

        case KEY_DOWN:
            startSelection();
            changeLine( 1 );
            mSelectionEnd = mCursorPos;
            break;

        case KEY_PAGE_DOWN:
            startSelection();
            changePage( 1 );
            mSelectionEnd = mCursorPos;
            break;

        case KEY_END:
            startSelection();
            if( mask & MASK_CONTROL )
            {
                setCursorPos(getLength());
            }
            else
            {
                endOfLine();
            }
            mSelectionEnd = mCursorPos;
            break;

        default:
            handled = false;
            break;
        }
    }

    // <FS:AW> Linux primary "clipboard" tainted by auto-selection
    //if( handled )
    //{
    //  // take selection to 'primary' clipboard
    //  updatePrimary();
    //}
    // </FS:AW> Linux primary "clipboard" tainted by auto-selection

    return handled;
}

bool LLTextEditor::handleNavigationKey(const KEY key, const MASK mask)
{
    bool handled = false;

    // Ignore capslock key
    if( MASK_NONE == mask )
    {
        handled = true;
        switch( key )
        {
        case KEY_UP:
            changeLine( -1 );
            break;

        case KEY_PAGE_UP:
            changePage( -1 );
            break;

        case KEY_HOME:
            startOfLine();
            break;

        case KEY_DOWN:
            changeLine( 1 );
            deselect();
            break;

        case KEY_PAGE_DOWN:
            changePage( 1 );
            break;

        case KEY_END:
            endOfLine();
            break;

        case KEY_LEFT:
            if( hasSelection() )
            {
                setCursorPos(llmin( mSelectionStart, mSelectionEnd ));
            }
            else
            {
                if( 0 < mCursorPos )
                {
                    setCursorPos(mCursorPos - 1);
                }
                else
                {
                    LLUI::getInstance()->reportBadKeystroke();
                }
            }
            break;

        case KEY_RIGHT:
            if( hasSelection() )
            {
                setCursorPos(llmax( mSelectionStart, mSelectionEnd ));
            }
            else
            {
                if( mCursorPos < getLength() )
                {
                    setCursorPos(mCursorPos + 1);
                }
                else
                {
                    LLUI::getInstance()->reportBadKeystroke();
                }
            }
            break;

        default:
            handled = false;
            break;
        }
    }

    if (handled)
    {
        deselect();
    }

    return handled;
}

void LLTextEditor::deleteSelection(bool group_with_next_op )
{
    if( getEnabled() && hasSelection() )
    {
        S32 pos = llmin( mSelectionStart, mSelectionEnd );
        S32 length = llabs( mSelectionStart - mSelectionEnd );

        remove( pos, length, group_with_next_op );

        deselect();
        setCursorPos(pos);
    }
}

// virtual
bool LLTextEditor::canCut() const
{
    return !mReadOnly && hasSelection();
}

// cut selection to clipboard
void LLTextEditor::cut()
{
    if( !canCut() )
    {
        return;
    }
    S32 left_pos = llmin( mSelectionStart, mSelectionEnd );
    S32 length = llabs( mSelectionStart - mSelectionEnd );
    LLClipboard::instance().copyToClipboard( getWText(), left_pos, length);
    deleteSelection( false );

    onKeyStroke();
}

bool LLTextEditor::canCopy() const
{
    return hasSelection();
}

// copy selection to clipboard
void LLTextEditor::copy()
{
    if( !canCopy() )
    {
        return;
    }
    S32 left_pos = llmin( mSelectionStart, mSelectionEnd );
    S32 length = llabs( mSelectionStart - mSelectionEnd );
    LLClipboard::instance().copyToClipboard(getWText(), left_pos, length);
}

bool LLTextEditor::canPaste() const
{
    return !mReadOnly && LLClipboard::instance().isTextAvailable();
}

// paste from clipboard
void LLTextEditor::paste()
{
    bool is_primary = false;
    pasteHelper(is_primary);
}

// paste from primary
void LLTextEditor::pastePrimary()
{
    bool is_primary = true;
    pasteHelper(is_primary);
}

// paste from primary (itsprimary==true) or clipboard (itsprimary==false)
void LLTextEditor::pasteHelper(bool is_primary)
{
    struct BoolReset
    {
        BoolReset(bool& value) : mValuePtr(&value) { *mValuePtr = false; }
        ~BoolReset() { *mValuePtr = true; }
        bool* mValuePtr;
    } reset(mParseOnTheFly);

    bool can_paste_it;
    if (is_primary)
    {
        can_paste_it = canPastePrimary();
    }
    else
    {
        can_paste_it = canPaste();
    }

    if (!can_paste_it)
    {
        return;
    }

    LLWString paste;
    LLClipboard::instance().pasteFromClipboard(paste, is_primary);

    if (paste.empty())
    {
        return;
    }

    // Delete any selected characters (the paste replaces them)
    if( (!is_primary) && hasSelection() )
    {
        deleteSelection(true);
    }

    // Clean up string (replace tabs and remove characters that our fonts don't support).
    LLWString clean_string(paste);
    cleanStringForPaste(clean_string);

    // <FS:ND> FIRE-4885; Truncate the text to mMaxTextByteLength.
    // Can safely do this here, otherwise it would done in '::insert', which is bad for performance, as ::insert is called once per line.
    // In theory text already in the editor should be taken into account too, but then text that would be overwriten would have to be considered aswell.
    if ( wstring_utf8_length(clean_string) > mMaxTextByteLength )
        clean_string = utf8str_to_wstring( utf8str_truncate( wstring_to_utf8str(clean_string), mMaxTextByteLength ) );
    // </FS:ND>

    // Insert the new text into the existing text.

    //paste text with linebreaks.
    pasteTextWithLinebreaks(clean_string);

    deselect();

    onKeyStroke();
}


// Clean up string (replace tabs and remove characters that our fonts don't support).
void LLTextEditor::cleanStringForPaste(LLWString & clean_string)
{
    std::string clean_string_utf = wstring_to_utf8str(clean_string);
    std::replace( clean_string_utf.begin(), clean_string_utf.end(), '\r', '\n');
    clean_string = utf8str_to_wstring(clean_string_utf);

    LLWStringUtil::replaceTabsWithSpaces(clean_string, SPACES_PER_TAB);
    if( mAllowEmbeddedItems )
    {
        const llwchar LF = 10;
        auto len = clean_string.length();
        for( size_t i = 0; i < len; i++ )
        {
            llwchar wc = clean_string[i];
            if( (wc < LLFontFreetype::FIRST_CHAR) && (wc != LF) )
            {
                clean_string[i] = LL_UNKNOWN_CHAR;
            }
            else if (wc >= FIRST_EMBEDDED_CHAR && wc <= LAST_EMBEDDED_CHAR)
            {
                clean_string[i] = pasteEmbeddedItem(wc);
            }
        }
    }
}


void LLTextEditor::pasteTextWithLinebreaks(LLWString & clean_string)
{
    std::basic_string<llwchar>::size_type start = 0;
    std::basic_string<llwchar>::size_type pos = clean_string.find('\n',start);

    while((pos != -1) && (pos != clean_string.length() -1))
    {
        if(pos!=start)
        {
            std::basic_string<llwchar> str = std::basic_string<llwchar>(clean_string,start,pos-start);
            setCursorPos(mCursorPos + insert(mCursorPos, str, true, LLTextSegmentPtr()));
        }
        addLineBreakChar(true);         // Add a line break and group with the next addition.

        start = pos+1;
        pos = clean_string.find('\n',start);
    }

    // <FS:Ansariel> FIRE-4314: Paste from clipboard shows block character if last character is a linefeed
    if (pos != start && pos == clean_string.length() - 1)
    {
        std::basic_string<llwchar> str = std::basic_string<llwchar>(clean_string,start,clean_string.length()-start-1);
        setCursorPos(mCursorPos + insert(mCursorPos, str, true, LLTextSegmentPtr()));
        addLineBreakChar(false);
    }
    else if (pos != start)
    //if (pos != start)
    // </FS:Ansariel>
    {
        std::basic_string<llwchar> str = std::basic_string<llwchar>(clean_string,start,clean_string.length()-start);
        setCursorPos(mCursorPos + insert(mCursorPos, str, false, LLTextSegmentPtr()));
    }
    else
    {
        addLineBreakChar(false);        // Add a line break and end the grouping.
    }
}

// copy selection to primary
void LLTextEditor::copyPrimary()
{
    if( !canCopy() )
    {
        return;
    }
    S32 left_pos = llmin( mSelectionStart, mSelectionEnd );
    S32 length = llabs( mSelectionStart - mSelectionEnd );
    LLClipboard::instance().copyToClipboard(getWText(), left_pos, length, true);
}

bool LLTextEditor::canPastePrimary() const
{
    return !mReadOnly && LLClipboard::instance().isTextAvailable(true);
}

void LLTextEditor::updatePrimary()
{
    if (canCopy())
    {
        copyPrimary();
    }
}

bool LLTextEditor::handleControlKey(const KEY key, const MASK mask)
{
    bool handled = false;

    if( mask & MASK_CONTROL )
    {
        handled = true;

        switch( key )
        {
        case KEY_HOME:
            if( mask & MASK_SHIFT )
            {
                startSelection();
                setCursorPos(0);
                mSelectionEnd = mCursorPos;
            }
            else
            {
                // Ctrl-Home, Ctrl-Left, Ctrl-Right, Ctrl-Down
                // all move the cursor as if clicking, so should deselect.
                deselect();
                startOfDoc();
            }
            break;

        case KEY_END:
            {
                if( mask & MASK_SHIFT )
                {
                    startSelection();
                }
                else
                {
                    // Ctrl-Home, Ctrl-Left, Ctrl-Right, Ctrl-Down
                    // all move the cursor as if clicking, so should deselect.
                    deselect();
                }
                endOfDoc();
                if( mask & MASK_SHIFT )
                {
                    mSelectionEnd = mCursorPos;
                }
                break;
            }

        case KEY_RIGHT:
            if( mCursorPos < getLength() )
            {
                // Ctrl-Home, Ctrl-Left, Ctrl-Right, Ctrl-Down
                // all move the cursor as if clicking, so should deselect.
                deselect();

                setCursorPos(nextWordPos(mCursorPos + 1));
            }
            break;


        case KEY_LEFT:
            if( mCursorPos > 0 )
            {
                // Ctrl-Home, Ctrl-Left, Ctrl-Right, Ctrl-Down
                // all move the cursor as if clicking, so should deselect.
                deselect();

                setCursorPos(prevWordPos(mCursorPos - 1));
            }
            break;

        default:
            handled = false;
            break;
        }
    }

    if (handled && !gFocusMgr.getMouseCapture())
    {
        updatePrimary();
    }

    return handled;
}


bool LLTextEditor::handleSpecialKey(const KEY key, const MASK mask)
    {
    bool handled = true;

    if (mReadOnly) return false;

    switch( key )
    {
    case KEY_INSERT:
        if (mask == MASK_NONE)
        {
            gKeyboard->toggleInsertMode();
        }
        break;

    case KEY_BACKSPACE:
        if( hasSelection() )
        {
            deleteSelection(false);
        }
        else
        if( 0 < mCursorPos )
        {
            // <FS> Ctrl-Backspace remove word
            //removeCharOrTab();
            if (mask == MASK_CONTROL)
                removeWord(true);
            else
                removeCharOrTab();
            // </FS>
        }
        else
        {
            LLUI::getInstance()->reportBadKeystroke();
        }
        break;

    // <FS> Ctrl-Backspace remove word
    case KEY_DELETE:
        if (getEnabled() && mask == MASK_CONTROL)
        {
            removeWord(false);
        }
        else
        {
            handled = false;
        }
        break;
    // </FS>

    case KEY_RETURN:
        if (mask == MASK_NONE)
        {
            if( hasSelection() && !mKeepSelectionOnReturn )
            {
                deleteSelection(false);
            }
            if (mAutoIndent)
            {
                autoIndent();
            }
        }
        else
        {
            handled = false;
            break;
        }
        break;

    case KEY_TAB:
        if (mask & MASK_CONTROL)
        {
            handled = false;
            break;
        }
        if( hasSelection() && selectionContainsLineBreaks() )
        {
            indentSelectedLines( (mask & MASK_SHIFT) ? -SPACES_PER_TAB : SPACES_PER_TAB );
        }
        else
        {
            if( hasSelection() )
            {
                deleteSelection(false);
            }

            S32 offset = getLineOffsetFromDocIndex(mCursorPos);

            // <FS:Ansariel> Allow Shift-Tab to tab-remove in text editors
            // Modified version from removeCharOrTab()
            if (mask & MASK_SHIFT)
            {
                S32 chars_to_remove = 0;

                LLWString text = getWText();
                if (mEnableTabRemove && text[mCursorPos - 1] == ' ')
                {
                    // Try to remove a "tab"
                    S32 offset = getLineOffsetFromDocIndex(mCursorPos);
                    if (offset > 0)
                    {
                        chars_to_remove = offset % SPACES_PER_TAB;
                        if (chars_to_remove == 0)
                        {
                            chars_to_remove = SPACES_PER_TAB;
                        }

                        for (S32 i = 0; i < chars_to_remove; i++)
                        {
                            if (text[mCursorPos - i - 1] != ' ')
                            {
                                // Fewer than a full tab's worth of spaces, so
                                // just delete a single character.
                                chars_to_remove = 1;
                                break;
                            }
                        }
                    }
                }

                for (S32 i = 0; i < chars_to_remove; i++)
                {
                    setCursorPos(mCursorPos - 1);
                    remove(mCursorPos, 1, false);
                }
            }
// <FS:Zi> FIRE-32175 - Linux/SDL2: Alt-Tab pushes a tab character into scripts and notecards
#if LL_SDL2
            // catch spurious ALT+Tab
            else if (!mask)
#else
// </FS:Zi>
            else
#endif  // <FS:Zi> FIRE-32175 - Linux/SDL2: Alt-Tab pushes a tab character into scripts and notecards
            {
            // </FS:Ansariel>
                S32 spaces_needed = SPACES_PER_TAB - (offset % SPACES_PER_TAB);
                for (S32 i = 0; i < spaces_needed; i++)
                {
                    addChar(' ');
                }
            } // <FS:Ansariel> Allow Shift-Tab to tab-remove in text editors
        }
        break;

    default:
        handled = false;
        break;
    }

    if (handled)
    {
        onKeyStroke();
    }
    return handled;
}


void LLTextEditor::unindentLineBeforeCloseBrace()
{
    if( mCursorPos >= 1 )
    {
        LLWString text = getWText();
        if( ' ' == text[ mCursorPos - 1 ] )
        {
            // <FS:Zi> FIRE-19959: Fix unindent after } when a previous line had a word wrap
            //S32 line = getLineNumFromDocIndex(mCursorPos, false);
            S32 line = getLineNumFromDocIndex(mCursorPos, true);
            // </FS:Zi>
            S32 line_start = getLineStart(line);

            // Jump over spaces in the current line
            while ((' ' == text[line_start]) && (line_start < mCursorPos))
            {
                line_start++;
            }

            // Make sure there is nothing but ' ' before the Brace we are unindenting
            if (line_start == mCursorPos)
            {
                removeCharOrTab();
            }
        }
    }
}


bool LLTextEditor::handleKeyHere(KEY key, MASK mask )
{
    bool    handled = false;

    // Special case for TAB.  If want to move to next field, report
    // not handled and let the parent take care of field movement.
    if (KEY_TAB == key && mTabsToNextField)
    {
        return mShowChatMentionPicker && LLChatMentionHelper::instance().handleKey(this, key, mask);
    }

    // <FS:Ansariel> FIRE-19933: Open context menu on context menu key press
    if (key == KEY_CONTEXT_MENU)
    {
        showContextMenu(getLocalRect().getCenterX(), getLocalRect().getCenterY(), false);
    }
    // </FS:Ansariel>

    if (mReadOnly && mScroller)
    {
        handled = (mScroller && mScroller->handleKeyHere( key, mask ))
                || handleSelectionKey(key, mask)
                || handleControlKey(key, mask);
    }
    else
    {
        if (!mReadOnly)
        {
            if ((mShowEmojiHelper && LLEmojiHelper::instance().handleKey(this, key, mask)) ||
                (mShowChatMentionPicker && LLChatMentionHelper::instance().handleKey(this, key, mask)))
            {
                return true;
            }
        }

        if (mEnableTooltipPaste &&
            LLToolTipMgr::instance().toolTipVisible() &&
            LLToolTipMgr::instance().isTooltipPastable() &&
            KEY_TAB == key)
        {   // Paste the first line of a tooltip into the editor
            std::string message;
            LLToolTipMgr::instance().getToolTipMessage(message);
            LLWString tool_tip_text(utf8str_to_wstring(message));

            if (tool_tip_text.size() > 0)
            {
                // Delete any selected characters (the tooltip text replaces them)
                if(hasSelection())
                {
                    deleteSelection(true);
                }

                std::basic_string<llwchar>::size_type pos = tool_tip_text.find('\n',0);
                if (pos != -1)
                {   // Extract the first line of the tooltip
                    tool_tip_text = std::basic_string<llwchar>(tool_tip_text, 0, pos);
                }

                // Add the text
                cleanStringForPaste(tool_tip_text);
                pasteTextWithLinebreaks(tool_tip_text);
                handled = true;
            }
        }
        else
        {   // Normal key handling
            handled = handleNavigationKey( key, mask )
                    || handleSelectionKey(key, mask)
                    || handleControlKey(key, mask)
                    || handleSpecialKey(key, mask);
        }
    }

    if( handled )
    {
        resetCursorBlink();
        needsScroll();

        if (mShowEmojiHelper)
        {
            // Dismiss the helper whenever we handled a key that it didn't
            LLEmojiHelper::instance().hideHelper(this);
        }
    }

    return handled;
}


bool LLTextEditor::handleUnicodeCharHere(llwchar uni_char)
{
    if ((uni_char < 0x20) || (uni_char == 0x7F)) // Control character or DEL
    {
        return false;
    }

    bool    handled = false;

    // Handle most keys only if the text editor is writeable.
    if( !mReadOnly )
    {
        if (mShowEmojiHelper && uni_char < 0x80 && LLEmojiHelper::instance().handleKey(this, (KEY)uni_char, MASK_NONE))
        {
            return true;
        }

        if( mAutoIndent && '}' == uni_char )
        {
            unindentLineBeforeCloseBrace();
        }

        // TODO: KLW Add auto show of tool tip on (
        addChar( uni_char );

        // Keys that add characters temporarily hide the cursor
        getWindow()->hideCursorUntilMouseMove();

        handled = true;
    }

    if( handled )
    {
        resetCursorBlink();

        // Most keystrokes will make the selection box go away, but not all will.
        deselect();

        onKeyStroke();
    }

    return handled;
}


// virtual
bool LLTextEditor::canDoDelete() const
{
    return !mReadOnly && ( !mPassDelete || ( hasSelection() || (mCursorPos < getLength())) );
}

void LLTextEditor::doDelete()
{
    if( !canDoDelete() )
    {
        return;
    }
    if( hasSelection() )
    {
        deleteSelection(false);
    }
    else
    if( mCursorPos < getLength() )
    {
        S32 i;
        S32 chars_to_remove = 1;
        LLWString text = getWText();
        if( (text[ mCursorPos ] == ' ') && (mCursorPos + SPACES_PER_TAB < getLength()) )
        {
            // Try to remove a full tab's worth of spaces
            S32 offset = getLineOffsetFromDocIndex(mCursorPos);
            chars_to_remove = SPACES_PER_TAB - (offset % SPACES_PER_TAB);
            if( chars_to_remove == 0 )
            {
                chars_to_remove = SPACES_PER_TAB;
            }

            for( i = 0; i < chars_to_remove; i++ )
            {
                if( text[mCursorPos + i] != ' ' )
                {
                    chars_to_remove = 1;
                    break;
                }
            }
        }

        for( i = 0; i < chars_to_remove; i++ )
        {
            setCursorPos(mCursorPos + 1);
            removeChar();
        }

    }

    onKeyStroke();
}

//----------------------------------------------------------------------------


void LLTextEditor::blockUndo()
{
    mBaseDocIsPristine = false;
    mLastCmd = NULL;
    std::for_each(mUndoStack.begin(), mUndoStack.end(), DeletePointer());
    mUndoStack.clear();
}

// virtual
bool LLTextEditor::canUndo() const
{
    return !mReadOnly && mLastCmd != NULL;
}

void LLTextEditor::undo()
{
    if( !canUndo() )
    {
        return;
    }
    deselect();
    S32 pos = 0;
    do
    {
        pos = mLastCmd->undo(this);
        undo_stack_t::iterator iter = std::find(mUndoStack.begin(), mUndoStack.end(), mLastCmd);
        if (iter != mUndoStack.end())
            ++iter;
        if (iter != mUndoStack.end())
            mLastCmd = *iter;
        else
            mLastCmd = NULL;

        } while( mLastCmd && mLastCmd->groupWithNext() );

        setCursorPos(pos);

    onKeyStroke();
}

bool LLTextEditor::canRedo() const
{
    return !mReadOnly && (mUndoStack.size() > 0) && (mLastCmd != mUndoStack.front());
}

void LLTextEditor::redo()
{
    if( !canRedo() )
    {
        return;
    }
    deselect();
    S32 pos = 0;
    do
    {
        if( !mLastCmd )
        {
            mLastCmd = mUndoStack.back();
        }
        else
        {
            undo_stack_t::iterator iter = std::find(mUndoStack.begin(), mUndoStack.end(), mLastCmd);
            if (iter != mUndoStack.begin())
                mLastCmd = *(--iter);
            else
                mLastCmd = NULL;
        }

            if( mLastCmd )
            {
                pos = mLastCmd->redo(this);
            }
        } while(
            mLastCmd &&
            mLastCmd->groupWithNext() &&
            (mLastCmd != mUndoStack.front()) );

        setCursorPos(pos);

    onKeyStroke();
}

void LLTextEditor::onFocusReceived()
{
    LLTextBase::onFocusReceived();
    updateAllowingLanguageInput();
}

void LLTextEditor::focusLostHelper()
{
    updateAllowingLanguageInput();

    // Route menu back to the default
    if( gEditMenuHandler == this )
    {
        gEditMenuHandler = NULL;
    }

    if (mCommitOnFocusLost)
    {
        onCommit();
    }

    // Make sure cursor is shown again
    getWindow()->showCursorFromMouseMove();
}

void LLTextEditor::onFocusLost()
{
    focusLostHelper();
    LLTextBase::onFocusLost();
}

void LLTextEditor::onCommit()
{
    setControlValue(getValue());
    LLTextBase::onCommit();
}

void LLTextEditor::setEnabled(bool enabled)
{
    // just treat enabled as read-only flag
    bool read_only = !enabled;
    if (read_only != mReadOnly)
    {
        //mReadOnly = read_only;
        LLTextBase::setReadOnly(read_only);
        updateSegments();
        updateAllowingLanguageInput();
    }
}

// <FS:Ansariel> FIRE-19933: Open context menu on context menu key press
//void LLTextEditor::showContextMenu(S32 x, S32 y)
void LLTextEditor::showContextMenu(S32 x, S32 y, bool set_cursor_pos)
// </FS:Ansariel>
{
    LLContextMenu* menu = static_cast<LLContextMenu*>(mContextMenuHandle.get());
    if (!menu)
    {
        llassert(LLMenuGL::sMenuContainer != NULL);
        menu = LLUICtrlFactory::createFromFile<LLContextMenu>("menu_text_editor.xml",
                                                                                LLMenuGL::sMenuContainer,
                                                                                LLMenuHolderGL::child_registry_t::instance());
        if(!menu)
        {
            LL_WARNS() << "Failed to create menu for LLTextEditor: " << getName() << LL_ENDL;
            return;
        }
        mContextMenuHandle = menu->getHandle();
    }

    // Route menu to this class
    // previously this was done in ::handleRightMoseDown:
    //if(hasTabStop())
    // setFocus(true)  - why? weird...
    // and then inside setFocus
    // ....
    //    gEditMenuHandler = this;
    // ....
    // but this didn't work in all cases and just weird...
    //why not here?
    // (all this was done for EXT-4443)

    gEditMenuHandler = this;

    S32 screen_x, screen_y;
    localPointToScreen(x, y, &screen_x, &screen_y);

    if (set_cursor_pos) // <FS:Ansariel> FIRE-19933: Open context menu on context menu key press
        setCursorAtLocalPos(x, y, false);
    if (hasSelection())
    {
        if ( (mCursorPos < llmin(mSelectionStart, mSelectionEnd)) || (mCursorPos > llmax(mSelectionStart, mSelectionEnd)) )
        {
            deselect();
        }
        else
        {
            setCursorPos(llmax(mSelectionStart, mSelectionEnd));
        }
    }

    bool use_spellcheck = getSpellCheck(), is_misspelled = false;
    if (use_spellcheck)
    {
        mSuggestionList.clear();

        // If the cursor is on a misspelled word, retrieve suggestions for it
        std::string misspelled_word = getMisspelledWord(mCursorPos);
        if ((is_misspelled = !misspelled_word.empty()))
        {
            LLSpellChecker::instance().getSuggestions(misspelled_word, mSuggestionList);
        }
    }

    menu->setItemVisible("Suggestion Separator", (use_spellcheck) && (!mSuggestionList.empty()));
    menu->setItemVisible("Add to Dictionary", (use_spellcheck) && (is_misspelled));
    menu->setItemVisible("Add to Ignore", (use_spellcheck) && (is_misspelled));
    menu->setItemVisible("Spellcheck Separator", (use_spellcheck) && (is_misspelled));
    menu->show(screen_x, screen_y, this);
}


void LLTextEditor::drawPreeditMarker()
{
    static LLUICachedControl<F32> preedit_marker_brightness ("UIPreeditMarkerBrightness", 0);
    static LLUICachedControl<S32> preedit_marker_gap ("UIPreeditMarkerGap", 0);
    static LLUICachedControl<S32> preedit_marker_position ("UIPreeditMarkerPosition", 0);
    static LLUICachedControl<S32> preedit_marker_thickness ("UIPreeditMarkerThickness", 0);
    static LLUICachedControl<F32> preedit_standout_brightness ("UIPreeditStandoutBrightness", 0);
    static LLUICachedControl<S32> preedit_standout_gap ("UIPreeditStandoutGap", 0);
    static LLUICachedControl<S32> preedit_standout_position ("UIPreeditStandoutPosition", 0);
    static LLUICachedControl<S32> preedit_standout_thickness ("UIPreeditStandoutThickness", 0);

    if (!hasPreeditString())
    {
        return;
    }

    const LLWString textString(getWText());
    const llwchar *text = textString.c_str();
    const S32 text_len = getLength();
    const S32 num_lines = getLineCount();

    S32 cur_line = getFirstVisibleLine();
    if (cur_line >= num_lines)
    {
        return;
    }

    const S32 line_height = mFont->getLineHeight();

    S32 line_start = getLineStart(cur_line);
    S32 line_y = mVisibleTextRect.mTop - line_height;
    while((mVisibleTextRect.mBottom <= line_y) && (num_lines > cur_line))
    {
        S32 next_start = -1;
        S32 line_end = text_len;

        if ((cur_line + 1) < num_lines)
        {
            next_start = getLineStart(cur_line + 1);
            line_end = next_start;
        }
        if ( text[line_end-1] == '\n' )
        {
            --line_end;
        }

        // Does this line contain preedits?
        if (line_start >= mPreeditPositions.back())
        {
            // We have passed the preedits.
            break;
        }
        if (line_end > mPreeditPositions.front())
        {
            for (U32 i = 0; i < mPreeditStandouts.size(); i++)
            {
                S32 left = mPreeditPositions[i];
                S32 right = mPreeditPositions[i + 1];
                if (right <= line_start || left >= line_end)
                {
                    continue;
                }

                line_info& line = mLineInfoList[cur_line];
                LLRect text_rect(line.mRect);
                text_rect.mRight = mDocumentView->getRect().getWidth(); // clamp right edge to document extents
                text_rect.translate(mDocumentView->getRect().mLeft, mDocumentView->getRect().mBottom); // adjust by scroll position

                S32 preedit_left = text_rect.mLeft;
                if (left > line_start)
                {
                    preedit_left += mFont->getWidth(text, line_start, left - line_start);
                }
                S32 preedit_right = text_rect.mLeft;
                if (right < line_end)
                {
                    preedit_right += mFont->getWidth(text, line_start, right - line_start);
                }
                else
                {
                    preedit_right += mFont->getWidth(text, line_start, line_end - line_start);
                }

                if (mPreeditStandouts[i])
                {
                    gl_rect_2d(preedit_left + preedit_standout_gap,
                               text_rect.mBottom + (S32)mFont->getDescenderHeight() - 1,
                               preedit_right - preedit_standout_gap - 1,
                               text_rect.mBottom + (S32)mFont->getDescenderHeight() - 1 - preedit_standout_thickness,
                               (mCursorColor.get() * preedit_standout_brightness + mWriteableBgColor.get() * (1 - preedit_standout_brightness)).setAlpha(1.0f));
                }
                else
                {
                    gl_rect_2d(preedit_left + preedit_marker_gap,
                               text_rect.mBottom + (S32)mFont->getDescenderHeight() - 1,
                               preedit_right - preedit_marker_gap - 1,
                               text_rect.mBottom + (S32)mFont->getDescenderHeight() - 1 - preedit_marker_thickness,
                               (mCursorColor.get() * preedit_marker_brightness + mWriteableBgColor.get() * (1 - preedit_marker_brightness)).setAlpha(1.0f));
                }
            }
        }

        // move down one line
        line_y -= line_height;
        line_start = next_start;
        cur_line++;
    }
}

void LLTextEditor::draw()
{
    {
        // pad clipping rectangle so that cursor can draw at full width
        // when at left edge of mVisibleTextRect
        LLRect clip_rect(mVisibleTextRect);
        clip_rect.stretch(1);
        LLLocalClipRect clip(clip_rect);
    }

    LLTextBase::draw();

    drawPreeditMarker();

    //RN: the decision was made to always show the orange border for keyboard focus but do not put an insertion caret
    // when in readonly mode
    mBorder->setKeyboardFocusHighlight( hasFocus() );// && !mReadOnly);
}

// Start or stop the editor from accepting text-editing keystrokes
// see also LLLineEditor
void LLTextEditor::setFocus( bool new_state )
{
    bool old_state = hasFocus();

    // Don't change anything if the focus state didn't change
    if (new_state == old_state) return;

    // Notify early if we are losing focus.
    if (!new_state)
    {
        getWindow()->allowLanguageTextInput(this, false);
    }

    LLTextBase::setFocus( new_state );

    if( new_state )
    {
        // Route menu to this class
        gEditMenuHandler = this;

        // Don't start the cursor flashing right away
        resetCursorBlink();
    }
    else
    {
        // Route menu back to the default
        if( gEditMenuHandler == this )
        {
            gEditMenuHandler = NULL;
        }

        endSelection();
    }
}

// public
void LLTextEditor::setCursorAndScrollToEnd()
{
    deselect();
    endOfDoc();
}

void LLTextEditor::getCurrentLineAndColumn( S32* line, S32* col, bool include_wordwrap )
{
    *line = getLineNumFromDocIndex(mCursorPos, include_wordwrap);
    *col = getLineOffsetFromDocIndex(mCursorPos, include_wordwrap);
}

void LLTextEditor::autoIndent()
{
    // Count the number of spaces in the current line
    // <FS:Zi> Fix indentation, always assume things can be word wrapped
    // S32 line = getLineNumFromDocIndex(mCursorPos, false);
    S32 line = getLineNumFromDocIndex(mCursorPos, true);
    // </FS:Zi>
    S32 line_start = getLineStart(line);
    S32 space_count = 0;
    S32 i;

    LLWString text = getWText();
    S32 offset = getLineOffsetFromDocIndex(mCursorPos);
    while(( ' ' == text[line_start] ) && (space_count < offset))
    {
        space_count++;
        line_start++;
    }

    // If we're starting a braced section, indent one level.
    if( (mCursorPos > 0) && (text[mCursorPos -1] == '{') )
    {
        space_count += SPACES_PER_TAB;
    }

    // Insert that number of spaces on the new line

    //appendLineBreakSegment(LLStyle::Params());//addChar( '\n' );
    addLineBreakChar();

    for( i = 0; i < space_count; i++ )
    {
        addChar( ' ' );
    }
}

// Inserts new text at the cursor position
void LLTextEditor::insertText(const std::string &new_text)
{
    bool enabled = getEnabled();
    setEnabled( true );

    // Delete any selected characters (the insertion replaces them)
    if( hasSelection() )
    {
        deleteSelection(true);
    }

    setCursorPos(mCursorPos + insert( mCursorPos, utf8str_to_wstring(new_text), false, LLTextSegmentPtr() ));

    setEnabled( enabled );
}

void LLTextEditor::insertText(LLWString &new_text)
{
    bool enabled = getEnabled();
    setEnabled( true );

    // Delete any selected characters (the insertion replaces them)
    if( hasSelection() )
    {
        deleteSelection(true);
    }

    setCursorPos(mCursorPos + insert( mCursorPos, new_text, false, LLTextSegmentPtr() ));

    setEnabled( enabled );
}

// <FS:Ansariel> Allow inserting a linefeed
void LLTextEditor::insertLinefeed()
{
    bool enabled = getEnabled();
    setEnabled(true);
    addLineBreakChar(false);
    setEnabled(enabled);
}
// </FS:Ansariel>

void LLTextEditor::appendWidget(const LLInlineViewSegment::Params& params, const std::string& text, bool allow_undo)
{
    // Save old state
    S32 selection_start = mSelectionStart;
    S32 selection_end = mSelectionEnd;
    bool was_selecting = mIsSelecting;
    S32 cursor_pos = mCursorPos;
    S32 old_length = getLength();
    bool cursor_was_at_end = (mCursorPos == old_length);

    deselect();

    setCursorPos(old_length);

    LLWString widget_wide_text = utf8str_to_wstring(text);

    LLTextSegmentPtr segment = new LLInlineViewSegment(params, old_length, old_length + static_cast<S32>(widget_wide_text.size()));
    insert(getLength(), widget_wide_text, false, segment);

    // Set the cursor and scroll position
    if( selection_start != selection_end )
    {
        mSelectionStart = selection_start;
        mSelectionEnd = selection_end;

        mIsSelecting = was_selecting;
        setCursorPos(cursor_pos);
    }
    else if( cursor_was_at_end )
    {
        setCursorPos(getLength());
    }
    else
    {
        setCursorPos(cursor_pos);
    }

    if (!allow_undo)
    {
        blockUndo();
    }
}

void LLTextEditor::removeTextFromEnd(S32 num_chars)
{
    if (num_chars <= 0) return;

    remove(getLength() - num_chars, num_chars, false);

    S32 len = getLength();
    setCursorPos (llclamp(mCursorPos, 0, len));
    mSelectionStart = llclamp(mSelectionStart, 0, len);
    mSelectionEnd = llclamp(mSelectionEnd, 0, len);

    needsScroll();
}

//----------------------------------------------------------------------------
void LLTextEditor::onSpellCheckPerformed()
{
    if (isPristine())
    {
        mBaseDocIsPristine = false;
    }
}

void LLTextEditor::makePristine()
{
    mPristineCmd = mLastCmd;
    mBaseDocIsPristine = !mLastCmd;

    // Create a clean partition in the undo stack.  We don't want a single command to extend from
    // the "pre-pristine" state to the "post-pristine" state.
    if( mLastCmd )
    {
        mLastCmd->blockExtensions();
    }
}

bool LLTextEditor::isPristine() const
{
    if( mPristineCmd )
    {
        return (mPristineCmd == mLastCmd);
    }
    else
    {
        // No undo stack, so check if the version before and commands were done was the original version
        return !mLastCmd && mBaseDocIsPristine;
    }
}

bool LLTextEditor::tryToRevertToPristineState()
{
    if( !isPristine() )
    {
        deselect();
        S32 i = 0;
        while( !isPristine() && canUndo() )
        {
            undo();
            i--;
        }

        while( !isPristine() && canRedo() )
        {
            redo();
            i++;
        }

        if( !isPristine() )
        {
            // failed, so go back to where we started
            while( i > 0 )
            {
                undo();
                i--;
            }
        }
    }

    return isPristine(); // true => success
}

void LLTextEditor::updateLinkSegments()
{
    LLWString wtext = getWText();

    // update any segments that contain a link
    for (segment_set_t::iterator it = mSegments.begin(); it != mSegments.end(); ++it)
    {
        LLTextSegment *segment = *it;
        if (segment && segment->getStyle() && segment->getStyle()->isLink())
        {
            LLStyleConstSP style = segment->getStyle();
            LLStyleSP new_style(new LLStyle(*style));
            LLWString url_label = wtext.substr(segment->getStart(), segment->getEnd()-segment->getStart());

            segment_set_t::const_iterator next_it = mSegments.upper_bound(segment);
            LLTextSegment *next_segment = *next_it;
            if (next_segment)
            {
                LLWString next_url_label = wtext.substr(next_segment->getStart(), next_segment->getEnd()-next_segment->getStart());
                std::string link_check = wstring_to_utf8str(url_label) + wstring_to_utf8str(next_url_label);
                LLUrlMatch match;

                if ( LLUrlRegistry::instance().findUrl(link_check, match))
                {
                    if(match.getQuery() == wstring_to_utf8str(next_url_label))
                    {
                        continue;
                    }
                }
            }

            // if the link's label (what the user can edit) is a valid Url,
            // then update the link's HREF to be the same as the label text.
            // This lets users edit Urls in-place.
            // <FS:Ansariel> FIRE-20054: Only update link's HREF to label text if the user can edit the text
            //if (acceptsTextInput() && LLUrlRegistry::instance().hasUrl(url_label))
            if (acceptsTextInput() && LLUrlRegistry::instance().hasUrl(url_label) && !getReadOnly())
            // </FS:Ansariel>
            {
                std::string new_url = wstring_to_utf8str(url_label);
                LLStringUtil::trim(new_url);
                new_style->setLinkHREF(new_url);
                LLStyleConstSP sp(new_style);
                segment->setStyle(sp);
            }
        }
    }
}



void LLTextEditor::onMouseCaptureLost()
{
    endSelection();
}

///////////////////////////////////////////////////////////////////
// Hack for Notecards

bool LLTextEditor::importBuffer(const char* buffer, S32 length )
{
    std::istringstream instream(buffer);

    // Version 1 format:
    //      Linden text version 1\n
    //      {\n
    //          <EmbeddedItemList chunk>
    //          Text length <bytes without \0>\n
    //          <text without \0> (text may contain ext_char_values)
    //      }\n

    char tbuf[MAX_STRING];  /* Flawfinder: ignore */

    S32 version = 0;
    instream.getline(tbuf, MAX_STRING);
    if( 1 != sscanf(tbuf, "Linden text version %d", &version) )
    {
        LL_WARNS() << "Invalid Linden text file header " << LL_ENDL;
        return false;
    }

    if( 1 != version )
    {
        LL_WARNS() << "Invalid Linden text file version: " << version << LL_ENDL;
        return false;
    }

    instream.getline(tbuf, MAX_STRING);
    if( 0 != sscanf(tbuf, "{") )
    {
        LL_WARNS() << "Invalid Linden text file format" << LL_ENDL;
        return false;
    }

    S32 text_len = 0;
    instream.getline(tbuf, MAX_STRING);
    if( 1 != sscanf(tbuf, "Text length %d", &text_len) )
    {
        LL_WARNS() << "Invalid Linden text length field" << LL_ENDL;
        return false;
    }

    if( text_len > mMaxTextByteLength )
    {
        LL_WARNS() << "Invalid Linden text length: " << text_len << LL_ENDL;
        return false;
    }

    bool success = true;

    char* text = new char[ text_len + 1];
    if (text == NULL)
    {
        LLError::LLUserWarningMsg::showOutOfMemory();
        LL_ERRS() << "Memory allocation failure." << LL_ENDL;
        return false;
    }
    instream.get(text, text_len + 1, '\0');
    text[text_len] = '\0';
    if( text_len != (S32)strlen(text) )/* Flawfinder: ignore */
    {
        LL_WARNS() << llformat("Invalid text length: %d != %d ",strlen(text),text_len) << LL_ENDL;/* Flawfinder: ignore */
        success = false;
    }

    instream.getline(tbuf, MAX_STRING);
    if( success && (0 != sscanf(tbuf, "}")) )
    {
        LL_WARNS() << "Invalid Linden text file format: missing terminal }" << LL_ENDL;
        success = false;
    }

    if( success )
    {
        // Actually set the text
        setText( LLStringExplicit(text) );
    }

    delete[] text;

    startOfDoc();
    deselect();

    return success;
}

bool LLTextEditor::exportBuffer(std::string &buffer )
{
    std::ostringstream outstream(buffer);

    outstream << "Linden text version 1\n";
    outstream << "{\n";

    outstream << llformat("Text length %d\n", getLength() );
    outstream << getText();
    outstream << "}\n";

    return true;
}

void LLTextEditor::updateAllowingLanguageInput()
{
    LLWindow* window = getWindow();
    if (!window)
    {
        // test app, no window available
        return;
    }
    if (hasFocus() && !mReadOnly)
    {
        window->allowLanguageTextInput(this, true);
    }
    else
    {
        window->allowLanguageTextInput(this, false);
    }
}

// Preedit is managed off the undo/redo command stack.

bool LLTextEditor::hasPreeditString() const
{
    return (mPreeditPositions.size() > 1);
}

void LLTextEditor::resetPreedit()
{
    if (hasSelection())
    {
        if (hasPreeditString())
        {
            LL_WARNS() << "Preedit and selection!" << LL_ENDL;
            deselect();
        }
        else
        {
            deleteSelection(true);
        }
    }
    if (hasPreeditString())
    {
        if (hasSelection())
        {
            LL_WARNS() << "Preedit and selection!" << LL_ENDL;
            deselect();
        }

        setCursorPos(mPreeditPositions.front());
        removeStringNoUndo(mCursorPos, mPreeditPositions.back() - mCursorPos);
        insertStringNoUndo(mCursorPos, mPreeditOverwrittenWString);

        mPreeditWString.clear();
        mPreeditOverwrittenWString.clear();
        mPreeditPositions.clear();

        // A call to updatePreedit should soon follow under a
        // normal course of operation, so we don't need to
        // maintain internal variables such as line start
        // positions now.
    }
}

void LLTextEditor::updatePreedit(const LLWString &preedit_string,
        const segment_lengths_t &preedit_segment_lengths, const standouts_t &preedit_standouts, S32 caret_position)
{
    // Just in case.
    if (mReadOnly)
    {
        return;
    }

    getWindow()->hideCursorUntilMouseMove();

    S32 insert_preedit_at = mCursorPos;

    mPreeditWString = preedit_string;
    mPreeditPositions.resize(preedit_segment_lengths.size() + 1);
    S32 position = insert_preedit_at;
    for (segment_lengths_t::size_type i = 0; i < preedit_segment_lengths.size(); i++)
    {
        mPreeditPositions[i] = position;
        position += preedit_segment_lengths[i];
    }
    mPreeditPositions.back() = position;

    if (LL_KIM_OVERWRITE == gKeyboard->getInsertMode())
    {
        mPreeditOverwrittenWString = getWText().substr(insert_preedit_at, mPreeditWString.length());
        removeStringNoUndo(insert_preedit_at, static_cast<S32>(mPreeditWString.length()));
    }
    else
    {
        mPreeditOverwrittenWString.clear();
    }

    segment_vec_t segments;
    //pass empty segments to let "insertStringNoUndo" make new LLNormalTextSegment and insert it, if needed.
    insertStringNoUndo(insert_preedit_at, mPreeditWString, &segments);

    mPreeditStandouts = preedit_standouts;

    setCursorPos(insert_preedit_at + caret_position);

    // Update of the preedit should be caused by some key strokes.
    resetCursorBlink();

    onKeyStroke();
}

bool LLTextEditor::getPreeditLocation(S32 query_offset, LLCoordGL *coord, LLRect *bounds, LLRect *control) const
{
    if (control)
    {
        LLRect control_rect_screen;
        localRectToScreen(mVisibleTextRect, &control_rect_screen);
        LLUI::getInstance()->screenRectToGL(control_rect_screen, control);
    }

    S32 preedit_left_position, preedit_right_position;
    if (hasPreeditString())
    {
        preedit_left_position = mPreeditPositions.front();
        preedit_right_position = mPreeditPositions.back();
    }
    else
    {
        preedit_left_position = preedit_right_position = mCursorPos;
    }

    const S32 query = (query_offset >= 0 ? preedit_left_position + query_offset : mCursorPos);
    if (query < preedit_left_position || query > preedit_right_position)
    {
        return false;
    }

    const S32 first_visible_line = getFirstVisibleLine();
    if (query < getLineStart(first_visible_line))
    {
        return false;
    }

    S32 current_line = first_visible_line;
    S32 current_line_start, current_line_end;
    for (;;)
    {
        current_line_start = getLineStart(current_line);
        current_line_end = getLineStart(current_line + 1);
        if (query >= current_line_start && query < current_line_end)
        {
            break;
        }
        if (current_line_start == current_line_end)
        {
            // We have reached on the last line.  The query position must be here.
            break;
        }
        current_line++;
    }

    const LLWString textString(getWText());
    const llwchar * const text = textString.c_str();
    const S32 line_height = mFont->getLineHeight();

    if (coord)
    {
        const S32 query_x = mVisibleTextRect.mLeft + mFont->getWidth(text, current_line_start, query - current_line_start);
        const S32 query_y = mVisibleTextRect.mTop - (current_line - first_visible_line) * line_height - line_height / 2;
        S32 query_screen_x, query_screen_y;
        localPointToScreen(query_x, query_y, &query_screen_x, &query_screen_y);
        LLUI::getInstance()->screenPointToGL(query_screen_x, query_screen_y, &coord->mX, &coord->mY);
    }

    if (bounds)
    {
        S32 preedit_left = mVisibleTextRect.mLeft;
        if (preedit_left_position > current_line_start)
        {
            preedit_left += mFont->getWidth(text, current_line_start, preedit_left_position - current_line_start);
        }

        S32 preedit_right = mVisibleTextRect.mLeft;
        if (preedit_right_position < current_line_end)
        {
            preedit_right += mFont->getWidth(text, current_line_start, preedit_right_position - current_line_start);
        }
        else
        {
            preedit_right += mFont->getWidth(text, current_line_start, current_line_end - current_line_start);
        }

        const S32 preedit_top = mVisibleTextRect.mTop - (current_line - first_visible_line) * line_height;
        const S32 preedit_bottom = preedit_top - line_height;

        const LLRect preedit_rect_local(preedit_left, preedit_top, preedit_right, preedit_bottom);
        LLRect preedit_rect_screen;
        localRectToScreen(preedit_rect_local, &preedit_rect_screen);
        LLUI::getInstance()->screenRectToGL(preedit_rect_screen, bounds);
    }

    return true;
}

void LLTextEditor::getSelectionRange(S32 *position, S32 *length) const
{
    if (hasSelection())
    {
        *position = llmin(mSelectionStart, mSelectionEnd);
        *length = llabs(mSelectionStart - mSelectionEnd);
    }
    else
    {
        *position = mCursorPos;
        *length = 0;
    }
}

void LLTextEditor::getPreeditRange(S32 *position, S32 *length) const
{
    if (hasPreeditString())
    {
        *position = mPreeditPositions.front();
        *length = mPreeditPositions.back() - mPreeditPositions.front();
    }
    else
    {
        *position = mCursorPos;
        *length = 0;
    }
}

void LLTextEditor::markAsPreedit(S32 position, S32 length)
{
    deselect();
    setCursorPos(position);
    if (hasPreeditString())
    {
        LL_WARNS() << "markAsPreedit invoked when hasPreeditString is true." << LL_ENDL;
    }
    mPreeditWString = LLWString( getWText(), position, length );
    if (length > 0)
    {
        mPreeditPositions.resize(2);
        mPreeditPositions[0] = position;
        mPreeditPositions[1] = position + length;
        mPreeditStandouts.resize(1);
        mPreeditStandouts[0] = false;
    }
    else
    {
        mPreeditPositions.clear();
        mPreeditStandouts.clear();
    }
    if (LL_KIM_OVERWRITE == gKeyboard->getInsertMode())
    {
        mPreeditOverwrittenWString = mPreeditWString;
    }
    else
    {
        mPreeditOverwrittenWString.clear();
    }
}

S32 LLTextEditor::getPreeditFontSize() const
{
    return ll_round((F32)mFont->getLineHeight() * LLUI::getScaleFactor().mV[VY]);
}

bool LLTextEditor::isDirty() const
{
    if(mReadOnly)
    {
        return false;
    }

    if( mPristineCmd )
    {
        return ( mPristineCmd == mLastCmd );
    }
    else
    {
        return ( NULL != mLastCmd );
    }
}

void LLTextEditor::setKeystrokeCallback(const keystroke_signal_t::slot_type& callback)
{
    mKeystrokeSignal.connect(callback);
}

void LLTextEditor::onKeyStroke()
{
    mKeystrokeSignal(this);

    mSpellCheckStart = mSpellCheckEnd = -1;
    mSpellCheckTimer.setTimerExpirySec(SPELLCHECK_DELAY);
}

//virtual
void LLTextEditor::clear()
{
    getViewModel()->setDisplay(LLWStringUtil::null);
    clearSegments();
}

bool LLTextEditor::canLoadOrSaveToFile()
{
    return !mReadOnly;
}

S32 LLTextEditor::spacesPerTab()
{
    return SPACES_PER_TAB;
}

LLWString LLTextEditor::getConvertedText() const
{
    LLWString text = getWText();
    S32 diff = 0;
    for (auto segment : mSegments)
    {
        if (segment && segment->getStyle() && segment->getStyle()->getDrawHighlightBg())
        {
            S32 seg_length = segment->getEnd() - segment->getStart();
            std::string slurl = segment->getStyle()->getLinkHREF();

            text.replace(segment->getStart() + diff, seg_length, utf8str_to_wstring(slurl));
            diff += (S32)slurl.size() - seg_length;
        }
    }
    return text;
}
