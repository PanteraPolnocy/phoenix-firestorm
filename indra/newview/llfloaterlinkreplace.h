/**
 * @file llfloaterlinkreplace.h
 * @brief Allows replacing link targets in inventory links
 * @author Ansariel Hiller
 *
 * $LicenseInfo:firstyear=2017&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2017, Linden Research, Inc.
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

#ifndef LL_FLOATERLINKREPLACE_H
#define LL_FLOATERLINKREPLACE_H

#include "llfloater.h"
#include "lleventtimer.h"
#include "lllineeditor.h"
#include "llinventoryfunctions.h"
#include "llviewerinventory.h"
#include "llcheckboxctrl.h" // <FS:Beq> FIRE-17695 - Delete links capability

class LLButton;
class LLTextBox;

class LLInventoryLinkReplaceDropTarget : public LLLineEditor
{
public:
    struct Params : public LLInitParam::Block<Params, LLLineEditor::Params>
    {
        Params()
        {}
    };

    LLInventoryLinkReplaceDropTarget(const Params& p)
        : LLLineEditor(p) {}
    ~LLInventoryLinkReplaceDropTarget() {}

    typedef boost::signals2::signal<void(const LLUUID& id)> item_dad_callback_t;
    boost::signals2::connection setDADCallback(const item_dad_callback_t::slot_type& cb)
    {
        return mDADSignal.connect(cb);
    }

    virtual bool postBuild()
    {
        setEnabled(false);
        return LLLineEditor::postBuild();
    }

    virtual bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                   EDragAndDropType cargo_type,
                                   void* cargo_data,
                                   EAcceptance* accept,
                                   std::string& tooltip_msg);

    LLUUID getItemID() const { return mItemID; }
    void setItem(LLInventoryItem* item);

private:
    LLUUID mItemID;

    item_dad_callback_t mDADSignal;
};


class LLFloaterLinkReplace : public LLFloater, LLEventTimer
{
    LOG_CLASS(LLFloaterLinkReplace);

public:
    LLFloaterLinkReplace(const LLSD& key);
    virtual ~LLFloaterLinkReplace();

    bool postBuild();
    virtual void onOpen(const LLSD& key);

    virtual bool tick();

private:
    void checkEnableStart();
    void onStartClicked();
    void onStartClickedResponse(const LLSD& notification, const LLSD& response);
    void onStopClicked();
    void decreaseOpenItemCount();
    void updateFoundLinks();
    void processBatch(LLInventoryModel::item_array_t items);

    static void linkCreatedCallback(LLHandle<LLFloaterLinkReplace> floater_handle, const LLUUID& old_item_id, const LLUUID& target_item_id,
                                    bool needs_wearable_ordering_update, bool needs_description_update, const LLUUID& outfit_folder_id);
    static void itemRemovedCallback(LLHandle<LLFloaterLinkReplace> floater_handle, const LLUUID& outfit_folder_id);

    void onSourceItemDrop(const LLUUID& source_item_id);
    void onTargetItemDrop(const LLUUID& target_item_id);
    void onDeleteOnlyToggle();// <FS:Beq> FIRE-17695 - Delete links capability

    LLInventoryLinkReplaceDropTarget*   mSourceEditor;
    LLInventoryLinkReplaceDropTarget*   mTargetEditor;
    LLButton*                           mStartBtn;
    LLButton*                           mRefreshBtn;
    LLButton*                           mStopBtn;
    LLTextBox*                          mStatusText;
    LLCheckBoxCtrl*                     mDeleteOnlyToggle;// <FS:Beq> FIRE-17695 - Delete links capability

    LLUUID  mSourceUUID;
    LLUUID  mTargetUUID;
    U32     mRemainingItems;
    U32     mBatchSize;
    U32     mActiveItems;

    LLInventoryModel::item_array_t  mRemainingInventoryItems;
    bool mDeleteOnly;// <FS:Beq> FIRE-17695 - Delete links capability
    bool mStopRequested;
};

#endif // LL_FLOATERLINKREPLACE_H
