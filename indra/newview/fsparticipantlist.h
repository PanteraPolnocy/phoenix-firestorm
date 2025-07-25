/**
 * @file fsparticipantlist.h
 * @brief FSParticipantList intended to update view(LLAvatarList) according to incoming messages
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
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

#ifndef FS_PARTICIPANTLIST_H
#define FS_PARTICIPANTLIST_H

#include "llviewerprecompiledheaders.h"
#include "llevent.h"
#include "llavatarlist.h" // for LLAvatarItemRecentSpeakerComparator
#include "llgroupmgr.h"
#include "lllistcontextmenu.h"

class LLSpeakerMgr;
class LLAvatarList;
class LLUICtrl;

class FSParticipantList
{
    LOG_CLASS(FSParticipantList);
public:
    enum EConversationType
    {
        CONV_UNKNOWN         = 0,
        CONV_PARTICIPANT     = 1,
        CONV_SESSION_NEARBY  = 2,
        CONV_SESSION_1_ON_1  = 3,
        CONV_SESSION_AD_HOC  = 4,
        CONV_SESSION_GROUP   = 5,
        CONV_SESSION_UNKNOWN = 6
    };

    typedef boost::function<bool (const LLUUID& speaker_id)> validate_speaker_callback_t;

    FSParticipantList(LLSpeakerMgr* data_source,
                      LLAvatarList* avatar_list,
                      bool use_context_menu = true,
                      bool exclude_agent = true,
                      bool can_toggle_icons = true);
    ~FSParticipantList();
    void setSpeakingIndicatorsVisible(bool visible);

    enum EParticipantSortOrder
    {
        E_SORT_BY_NAME = 0,
        E_SORT_BY_RECENT_SPEAKERS = 1,
    };

    /**
     * Adds specified avatar ID to the existing list if it is not Agent's ID
     *
     * @param[in] avatar_id - Avatar UUID to be added into the list
     */
    void addAvatarIDExceptAgent(const LLUUID& avatar_id);

    /**
     * Set and sort Avatarlist by given order
     */
    void setSortOrder(EParticipantSortOrder order = E_SORT_BY_NAME);
    const EParticipantSortOrder getSortOrder() const;

    /**
     * Refreshes the participant list.
     */
    void update();

    /**
     * Set a callback to be called before adding a speaker. Invalid speakers will not be added.
     *
     * If the callback is unset all speakers are considered as valid.
     *
     * @see onAddItemEvent()
     */
    void setValidateSpeakerCallback(validate_speaker_callback_t cb);

    EConversationType const getType() const { return mConvType; }

    uuid_vec_t getAvatarIds() const;

protected:
    /**
     * LLSpeakerMgr event handlers
     */
    bool onAddItemEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    bool onRemoveItemEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    bool onClearListEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    bool onModeratorUpdateEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    bool onSpeakerMuteEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);

    /**
     * Sorts the Avatarlist by stored order
     */
    void sort();

    /**
     * List of listeners implementing LLOldEvents::LLSimpleListener.
     * There is no way to handle all the events in one listener as LLSpeakerMgr registers
     * listeners in such a way that one listener can handle only one type of event
     **/
    class BaseSpeakerListener : public LLOldEvents::LLSimpleListener
    {
    public:
        BaseSpeakerListener(FSParticipantList& parent) : mParent(parent) {}
    protected:
        FSParticipantList& mParent;
    };

    class SpeakerAddListener : public BaseSpeakerListener
    {
    public:
        SpeakerAddListener(FSParticipantList& parent) : BaseSpeakerListener(parent) {}
        /*virtual*/ bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    };

    class SpeakerRemoveListener : public BaseSpeakerListener
    {
    public:
        SpeakerRemoveListener(FSParticipantList& parent) : BaseSpeakerListener(parent) {}
        /*virtual*/ bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    };

    class SpeakerClearListener : public BaseSpeakerListener
    {
    public:
        SpeakerClearListener(FSParticipantList& parent) : BaseSpeakerListener(parent) {}
        /*virtual*/ bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    };

    class SpeakerModeratorUpdateListener : public BaseSpeakerListener
    {
    public:
        SpeakerModeratorUpdateListener(FSParticipantList& parent) : BaseSpeakerListener(parent) {}
        /*virtual*/ bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    };

    class SpeakerMuteListener : public BaseSpeakerListener
    {
    public:
        SpeakerMuteListener(FSParticipantList& parent) : BaseSpeakerListener(parent) {}

        /*virtual*/ bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata);
    };

    /**
     * Menu used in the participant list.
     */
    class FSParticipantListMenu : public LLListContextMenu
    {
    public:
        FSParticipantListMenu(FSParticipantList& parent):mParent(parent){};
        /*virtual*/ LLContextMenu* createMenu();
        /*virtual*/ void show(LLView* spawning_view, const uuid_vec_t& uuids, S32 x, S32 y);
    protected:
        FSParticipantList& mParent;
    private:
        bool enableContextMenuItem(const LLSD& userdata);
        bool enableModerateContextMenuItem(const LLSD& userdata);
        bool checkContextMenuItem(const LLSD& userdata);

        void sortParticipantList(const LLSD& userdata);
        void allowTextChat(const LLSD& userdata);
        void toggleMute(const LLSD& userdata, U32 flags);
        void toggleMuteText(const LLSD& userdata);
        void toggleMuteVoice(const LLSD& userdata);

        /**
         * Return true if Agent is group moderator(and moderator of group call).
         */
        bool isGroupModerator();

        bool hasAbilityToBan();
        bool canBanSelectedMember(const LLUUID& participant_uuid);
        void banSelectedMember(const LLUUID& participant_uuid);

        // Voice moderation support
        /**
         * Check whether specified by argument avatar is muted for group chat or not.
         */
        bool isMuted(const LLUUID& avatar_id);

        /**
         * Processes Voice moderation menu items.
         *
         * It calls either moderateVoiceParticipant() or moderateVoiceParticipant() depend on
         * passed parameter.
         *
         * @param userdata can be "selected" or "others".
         *
         * @see moderateVoiceParticipant()
         * @see moderateVoiceAllParticipants()
         */
        void moderateVoice(const LLSD& userdata);

        /**
         * Mutes/Unmutes avatar for current group voice chat.
         *
         * It only marks avatar as muted for session and does not use local Agent's Block list.
         * It does not mute Agent itself.
         *
         * @param[in] avatar_id UUID of avatar to be processed
         * @param[in] unmute if true - specified avatar will be muted, otherwise - unmuted.
         *
         * @see moderateVoiceAllParticipants()
         */
        void moderateVoiceParticipant(const LLUUID& avatar_id, bool unmute);

        /**
         * Mutes/Unmutes all avatars for current group voice chat.
         *
         * It only marks avatars as muted for session and does not use local Agent's Block list.
         *
         * @param[in] unmute if true - avatars will be muted, otherwise - unmuted.
         *
         * @see moderateVoiceParticipant()
         */
        void moderateVoiceAllParticipants(bool unmute);

        static void confirmMuteAllCallback(const LLSD& notification, const LLSD& response);

        void handleAddToContactSet();
    };

    /**
     * Comparator for comparing avatar items by last spoken time
     */
    class LLAvatarItemRecentSpeakerComparator : public LLAvatarItemNameComparator, public LLRefCount
    {
        LOG_CLASS(LLAvatarItemRecentSpeakerComparator);
    public:
        LLAvatarItemRecentSpeakerComparator(FSParticipantList& parent):mParent(parent){};
        virtual ~LLAvatarItemRecentSpeakerComparator() {};
    protected:
        virtual bool doCompare(const LLAvatarListItem* avatar_item1, const LLAvatarListItem* avatar_item2) const;
    private:
        FSParticipantList& mParent;
    };

private:
    void onAvatarListDoubleClicked(LLUICtrl* ctrl);
    void onAvatarListRefreshed(LLUICtrl* ctrl, const LLSD& param);

    /**
     * Adjusts passed participant to work properly.
     *
     * Adds SpeakerMuteListener to process moderation actions.
     */
    void adjustParticipant(const LLUUID& speaker_id);

    bool isHovered();

    LLSpeakerMgr*       mSpeakerMgr;
    LLAvatarList*       mAvatarList;

    std::set<LLUUID>    mModeratorList;
    std::set<LLUUID>    mModeratorToRemoveList;

    LLPointer<SpeakerAddListener>               mSpeakerAddListener;
    LLPointer<SpeakerRemoveListener>            mSpeakerRemoveListener;
    LLPointer<SpeakerClearListener>             mSpeakerClearListener;
    LLPointer<SpeakerModeratorUpdateListener>   mSpeakerModeratorListener;
    LLPointer<SpeakerMuteListener>              mSpeakerMuteListener;

    FSParticipantListMenu*    mParticipantListMenu;

    /**
     * This field manages an adding  a new avatar_id in the mAvatarList
     * If true, then agent_id wont  be added into mAvatarList
     * Also by default this field is controlling a sort procedure, @c sort()
     */
    bool mExcludeAgent;

    // boost::connections
    boost::signals2::connection mAvatarListDoubleClickConnection;
    boost::signals2::connection mAvatarListRefreshConnection;
    boost::signals2::connection mAvatarListReturnConnection;
    boost::signals2::connection mAvatarListToggleIconsConnection;

    LLPointer<LLAvatarItemRecentSpeakerComparator> mSortByRecentSpeakers;
    validate_speaker_callback_t mValidateSpeakerCallback;

    EConversationType mConvType;
};

#endif // FS_PARTICIPANTLIST_H
