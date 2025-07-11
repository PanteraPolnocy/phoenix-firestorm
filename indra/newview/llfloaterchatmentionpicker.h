/**
 * @file llfloaterchatmentionpicker.h
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2025, Linden Research, Inc.
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

#ifndef LLFLOATERCHATMENTIONPICKER_H
#define LLFLOATERCHATMENTIONPICKER_H

#include "llfloater.h"

class LLAvatarList;
class FSChatParticipants;

class LLFloaterChatMentionPicker : public LLFloater
{
public:
    LLFloaterChatMentionPicker(const LLSD& key);

    virtual bool postBuild() override;
    virtual void goneFromFront() override;

    void buildAvatarList();

    static uuid_vec_t getParticipantIds();
    static void updateSessionID(LLUUID session_id);
    static void updateAvatarList(const uuid_vec_t& avatar_ids);

    // <FS:Ansariel> [FS communication UI]
    static void updateParticipantSource(FSChatParticipants* source);
    static void removeParticipantSource(FSChatParticipants* source);

private:

    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    virtual bool handleKey(KEY key, MASK mask, bool called_from_parent) override;
    void selectResident(const LLUUID& id);

    static LLUUID sSessionID;
    LLAvatarList* mAvatarList;

    // <FS:Ansariel> [FS communication UI]
    static FSChatParticipants* sParticipantSource;
};

#endif
