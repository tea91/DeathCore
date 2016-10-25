/*
 * Copyright (C) 2016 DeathCore <http://www.noffearrdeathproject.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "SocialMgr.h"
#include "SpellAuras.h"
#include "Util.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

class Aura;

/* differeces from off:
    -you can uninvite yourself - is is useful
    -you can accept invitation even if leader went offline
*/
/* todo:
    -group_destroyed msg is sent but not shown
    -reduce xp gaining when in raid group
    -quest sharing has to be corrected
    -FIX sending PartyMemberStats
*/

void WorldSession::SendPartyResult(PartyOperation operation, const std::string& member, PartyResult res, uint32 val /* = 0 */)
{
    WorldPacket data(SMSG_PARTY_COMMAND_RESULT, 4 + member.size() + 1 + 4 + 4 + 8);
    data << uint32(operation);
    data << member;
    data << uint32(res);
    data << uint32(val);                                // LFD cooldown related (used with ERR_PARTY_LFG_BOOT_COOLDOWN_S and ERR_PARTY_LFG_BOOT_NOT_ELIGIBLE_S)
    data << uint64(0);                                  // player who caused error (in some cases).

    SendPacket(&data);
}

void WorldSession::SendGroupInviteNotification(const std::string& inviterName, bool inGroup)
{
    TC_LOG_DEBUG("network", "WORLD: sending SMSG_GROUP_INVITE");
    
    ObjectGuid invitedGuid = GetPlayer()->GetGUID();

    WorldPacket data(SMSG_GROUP_INVITE, 6 + 1 + 8 + 8 + 4 + 4 + 4 + inviterName.size());
    data.WriteBits(0, 8);
    data.WriteBits(0, 8);
    data.WriteBit(invitedGuid[2]);
    data.WriteBit(0);
    data.WriteBits(inviterName.size(), 6);              // inviter name length
    data.WriteGuidMask(invitedGuid, 7, 5);
    data.WriteBit(!inGroup);                            // inverse already in group
    data.WriteBit(0);                                   // auto decline
    data.WriteBit(invitedGuid[1]);
    data.WriteBit(0);                                   // cross realm invite (includes hyphen between inviter and server name)
    data.WriteBit(0);                                   // realm transfer warning("Accepting this invitation may transfer you to another realm")
    data.WriteBits(0, 22);                              // counter
    data.WriteGuidMask(invitedGuid, 3, 0, 4, 6);
    data.FlushBits();

    data.WriteGuidBytes(invitedGuid, 6, 7, 2, 0);
    data << uint64(0);
    data << uint32(0);
    data << uint32(0);
    data.WriteGuidBytes(invitedGuid, 1, 5, 4);
    data << int32(0);
    data.WriteString(inviterName);
    data.WriteByteSeq(invitedGuid[3]);
    data << uint32(0);

    /*for (int i = 0; i < counter; i++)
        data << int32(0);*/

    SendPacket(&data);
}

void WorldSession::HandleGroupInviteOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_INVITE");

    ObjectGuid crossRealmGuid;                                      // unused

    recvData.read_skip<uint32>();                                   // Non-zero in cross realm invites
    recvData.read_skip<uint8>();                                    // Unknown
    recvData.read_skip<uint32>();                                   // Always 0

    crossRealmGuid[7] = recvData.ReadBit();
    uint8 realmLen = recvData.ReadBits(9);
    crossRealmGuid[3] = recvData.ReadBit();
    uint8 nameLen = recvData.ReadBits(9);
    recvData.ReadGuidMask(crossRealmGuid, 2, 5, 4, 0, 1, 6);

    recvData.ReadGuidBytes(crossRealmGuid, 7, 6, 0, 4);
    std::string realmName = recvData.ReadString(realmLen);          // unused
    recvData.ReadGuidBytes(crossRealmGuid, 1, 2, 3);
    std::string memberName = recvData.ReadString(nameLen);
    recvData.ReadByteSeq(crossRealmGuid[5]);

    // attempt add selected player

    // cheating
    if (!normalizePlayerName(memberName))
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    Player* player = sObjectAccessor->FindPlayerByName(memberName);

    // no player
    if (!player)
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    // restrict invite to GMs
    if (!sWorld->getBoolConfig(CONFIG_ALLOW_GM_GROUP) && !GetPlayer()->IsGameMaster() && player->IsGameMaster())
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    // can't group with
    if (!GetPlayer()->IsGameMaster() && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) && GetPlayer()->GetTeam() != player->GetTeam())
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_PLAYER_WRONG_FACTION);
        return;
    }

    if (GetPlayer()->GetInstanceId() != 0 && player->GetInstanceId() != 0 && GetPlayer()->GetInstanceId() != player->GetInstanceId() && GetPlayer()->GetMapId() == player->GetMapId())
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_TARGET_NOT_IN_INSTANCE_S);
        return;
    }

    // just ignore us
    if (player->GetInstanceId() != 0 && player->GetDungeonDifficulty() != GetPlayer()->GetDungeonDifficulty())
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_IGNORING_YOU_S);
        return;
    }

    if (player->GetSocial()->HasIgnore(GetPlayer()->GetGUIDLow()))
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_IGNORING_YOU_S);
        return;
    }

    Group* group = GetPlayer()->GetGroup();
    if (group && group->isBGGroup())
        group = GetPlayer()->GetOriginalGroup();

    Group* group2 = player->GetGroup();
    if (group2 && group2->isBGGroup())
        group2 = player->GetOriginalGroup();
    // player already in another group or invited
    if (group2 || player->GetGroupInvite())
    {
        SendPartyResult(PARTY_OP_INVITE, memberName, ERR_ALREADY_IN_GROUP_S);

        if (group2)
        {
            // tell the player that they were invited but it failed as they were already in a group
            player->GetSession()->SendGroupInviteNotification(GetPlayer()->GetName(), true);
        }

        return;
    }

    if (group)
    {
        // not have permissions for invite
        if (!group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
        {
            SendPartyResult(PARTY_OP_INVITE, "", ERR_NOT_LEADER);
            return;
        }
        // not have place
        if (group->IsFull())
        {
            SendPartyResult(PARTY_OP_INVITE, "", ERR_GROUP_FULL);
            return;
        }
    }

    // ok, but group not exist, start a new group
    // but don't create and save the group to the DB until
    // at least one person joins
    if (!group)
    {
        group = new Group;
        // new group: if can't add then delete
        if (!group->AddLeaderInvite(GetPlayer()))
        {
            delete group;
            return;
        }
        if (!group->AddInvite(player))
        {
            delete group;
            return;
        }
    }
    else
    {
        // already existed group: if can't add then just leave
        if (!group->AddInvite(player))
        {
            return;
        }
    }

    // ok, we do it
    player->GetSession()->SendGroupInviteNotification(GetPlayer()->GetName(), false);

    SendPartyResult(PARTY_OP_INVITE, memberName, ERR_PARTY_RESULT_OK);
}

void WorldSession::HandleGroupInviteResponseOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_INVITE_RESPONSE");

    recvData.read_skip<uint8>();                // Unknown
    bool unknown = recvData.ReadBit();          // Unknown
    bool accept = recvData.ReadBit();

    /*if (unknown)
        recvData.read_skip<uint32>();*/

    Group* group = GetPlayer()->GetGroupInvite();

    if (!group)
        return;

    if (accept)
    {
        // Remove player from invitees in any case
        group->RemoveInvite(GetPlayer());

        if (group->GetLeaderGUID() == GetPlayer()->GetGUID())
        {
            TC_LOG_ERROR("network", "HandleGroupAcceptOpcode: player %s(%d) tried to accept an invite to his own group", GetPlayer()->GetName().c_str(), GetPlayer()->GetGUIDLow());
            return;
        }

        // Group is full
        if (group->IsFull())
        {
            SendPartyResult(PARTY_OP_INVITE, "", ERR_GROUP_FULL);
            return;
        }

        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());

        // Forming a new group, create it
        if (!group->IsCreated())
        {
            // This can happen if the leader is zoning. To be removed once delayed actions for zoning are implemented
            if (!leader)
            {
                group->RemoveAllInvites();
                return;
            }

            // If we're about to create a group there really should be a leader present
            ASSERT(leader);
            group->RemoveInvite(leader);
            group->Create(leader);
            sGroupMgr->AddGroup(group);
        }

        // Everything is fine, do it, PLAYER'S GROUP IS SET IN ADDMEMBER!!!
        if (!group->AddMember(GetPlayer()))
            return;

        group->BroadcastGroupUpdate();
    }
    else
    {
        // Remember leader if online (group pointer will be invalid if group gets disbanded)
        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());

        // uninvite, group can be deleted
        GetPlayer()->UninviteFromGroup();

        if (!leader || !leader->GetSession())
            return;

        // report
        WorldPacket data(SMSG_GROUP_DECLINE, GetPlayer()->GetName().size());
        data << GetPlayer()->GetName();
        leader->GetSession()->SendPacket(&data);
    }
}

void WorldSession::HandleGroupUninviteGuidOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_UNINVITE_GUID");

    ObjectGuid guid;

    recvData.read_skip<uint8>();

    recvData.ReadGuidMask(guid, 6, 4, 3, 2, 0, 1, 7, 5);

    uint8 reasonLen = recvData.ReadBits(8);
    std::string reason = recvData.ReadString(reasonLen);
    recvData.ReadGuidBytes(guid, 5, 6, 1, 4, 3, 2, 7, 0);

    //can't uninvite yourself
    if (guid == GetPlayer()->GetGUID())
    {
        TC_LOG_ERROR("network", "WorldSession::HandleGroupUninviteGuidOpcode: leader %s(%d) tried to uninvite himself from the group.",
            GetPlayer()->GetName().c_str(), GetPlayer()->GetGUIDLow());
        return;
    }

    PartyResult res = GetPlayer()->CanUninviteFromGroup();
    if (res != ERR_PARTY_RESULT_OK)
    {
        SendPartyResult(PARTY_OP_UNINVITE, "", res);
        return;
    }

    Group* grp = GetPlayer()->GetGroup();
    if (!grp)
        return;

    if (grp->IsLeader(guid))
    {
        SendPartyResult(PARTY_OP_UNINVITE, "", ERR_NOT_LEADER);
        return;
    }

    if (grp->IsMember(guid))
    {
        Player::RemoveFromGroup(grp, guid, GROUP_REMOVEMETHOD_KICK, GetPlayer()->GetGUID(), reason.c_str());
        return;
    }

    if (Player* player = grp->GetInvited(guid))
    {
        player->UninviteFromGroup();
        return;
    }

    SendPartyResult(PARTY_OP_UNINVITE, "", ERR_TARGET_NOT_IN_GROUP_S);
}

void WorldSession::HandleGroupSetLeaderOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_SET_LEADER");

    ObjectGuid guid;

    recvData.read_skip<uint8>();

    recvData.ReadGuidMask(guid, 1, 7, 0, 2, 5, 3, 4, 6);

    recvData.ReadGuidBytes(guid, 1, 5, 7, 6, 0, 2, 4, 3);

    Player* player = ObjectAccessor::FindPlayer(guid);
    Group* group = GetPlayer()->GetGroup();

    if (!group || !player)
        return;

    if (!group->IsLeader(GetPlayer()->GetGUID()) || player->GetGroup() != group)
        return;

    // Everything's fine, accepted.
    group->ChangeLeader(guid);
    group->SendUpdate();

}

void WorldSession::HandleGroupSetRolesOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_SET_ROLES");

    uint32 newRole;
    ObjectGuid targetGuid;

    recvData.read_skip<uint8>();
    recvData >> newRole;

    recvData.ReadGuidMask(targetGuid, 2, 0, 7, 4, 1, 3, 6, 5);

    recvData.ReadGuidBytes(targetGuid, 1, 5, 2, 6, 7, 0, 4, 3);

    Player* tPlayer = ObjectAccessor::FindPlayer(targetGuid);
    Group* group = GetPlayer()->GetGroup();

    if (!tPlayer || !group)
        return;

    if (group != tPlayer->GetGroup())
        return;

    ObjectGuid assignerGuid = GetPlayer()->GetGUID();

    WorldPacket data(SMSG_GROUP_SET_ROLE, 1 + 8 + 1 + 8 + 4 + 1 + 4);
    data.WriteBit(assignerGuid[1]);
    data.WriteGuidMask(targetGuid, 7, 6, 4, 1, 0);
    data.WriteGuidMask(assignerGuid, 0, 7);
    data.WriteBit(targetGuid[3]);
    data.WriteBit(assignerGuid[6]);
    data.WriteBit(targetGuid[2]);
    data.WriteGuidMask(assignerGuid, 4, 5, 2);
    data.WriteBit(targetGuid[5]);
    data.WriteBit(assignerGuid[3]);

    data.WriteGuidBytes(assignerGuid, 1, 6, 2);
    data.WriteByteSeq(targetGuid[3]);
    data << uint32(group->GetMemberRole(targetGuid));
    data.WriteByteSeq(assignerGuid[7]);
    data.WriteByteSeq(targetGuid[5]);
    data.WriteByteSeq(assignerGuid[3]);
    data.WriteGuidBytes(targetGuid, 4, 7);
    data.WriteByteSeq(assignerGuid[5]);
    data.WriteGuidBytes(targetGuid, 6, 2, 1, 0);
    data.WriteByteSeq(assignerGuid[4]);
    data << uint8(0);                           // unknown
    data.WriteByteSeq(assignerGuid[0]);
    data << uint32(newRole);

    group->BroadcastPacket(&data, false);
    group->SetMemberRole(targetGuid, newRole);
    group->SendUpdate();
}

void WorldSession::HandleGroupDisbandOpcode(WorldPacket& /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_DISBAND");

    Group* grp = GetPlayer()->GetGroup();
    if (!grp)
        return;

    if (_player->InBattleground())
    {
        SendPartyResult(PARTY_OP_INVITE, "", ERR_INVITE_RESTRICTED);
        return;
    }

    /** error handling **/
    /********************/

    // everything's fine, do it
    SendPartyResult(PARTY_OP_LEAVE, GetPlayer()->GetName(), ERR_PARTY_RESULT_OK);

    GetPlayer()->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
}

void WorldSession::HandleLootMethodOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_LOOT_METHOD");

    ObjectGuid lootMaster;
    uint8 lootMethod;
    uint8 lootThreshold;

    recvData >> lootThreshold;
    recvData >> lootMethod;
    recvData.read_skip<uint32>();

    recvData.ReadGuidMask(lootMaster, 7, 1, 2, 0, 4, 5, 6, 3);

    recvData.ReadGuidBytes(lootMaster, 7, 1, 3, 4, 6, 5, 0, 2);

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    /** error handling **/
    if (!group->IsLeader(GetPlayer()->GetGUID()))
        return;
    /********************/

    // everything's fine, do it
    group->SetLootMethod((LootMethod)lootMethod);
    group->SetLooterGuid(lootMaster);
    group->SetLootThreshold((ItemQualities)lootThreshold);
    group->SendUpdate();
}

void WorldSession::HandleLootRoll(WorldPacket& recvData)
{
    uint64 guid;
    uint32 itemSlot;
    uint8  rollType;
    recvData >> guid;                  // guid of the item rolled
    recvData >> itemSlot;
    recvData >> rollType;              // 0: pass, 1: need, 2: greed

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    group->CountRollVote(GetPlayer()->GetGUID(), guid, rollType);

    switch (rollType)
    {
        case ROLL_NEED:
            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED, 1);
            break;
        case ROLL_GREED:
            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED, 1);
            break;
    }
}

void WorldSession::HandleMinimapPingOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_MINIMAP_PING");

    if (!GetPlayer()->GetGroup())
        return;

    float x, y;
    recvData >> y;
    recvData >> x;
    recvData.read_skip<uint8>();

    //TC_LOG_DEBUG("misc", "Received opcode MSG_MINIMAP_PING X: %f, Y: %f", x, y);

    /** error handling **/
    /********************/

    ObjectGuid guid = GetPlayer()->GetGUID();

    // everything's fine, do it
    WorldPacket data(SMSG_MINIMAP_PING, 1 + 8 + 4 + 4);
    data << float(y);
    data << float(x);

    data.WriteGuidMask(guid, 0, 5, 2, 7, 1, 3, 6, 4);

    data.WriteGuidBytes(guid, 6, 5, 7, 2, 0, 3, 1, 4);

    GetPlayer()->GetGroup()->BroadcastPacket(&data, true, -1, GetPlayer()->GetGUID());
}

void WorldSession::HandleRandomRollOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_RANDOM_ROLL");

    uint32 minimum, maximum, roll;
    recvData >> maximum;
    recvData >> minimum;
    recvData.read_skip<uint8>();

    /** error handling **/
    if (minimum > maximum || maximum > 10000)                // < 32768 for urand call
        return;
    /********************/

    // everything's fine, do it
    roll = urand(minimum, maximum);

    //TC_LOG_DEBUG("misc", "ROLL: MIN: %u, MAX: %u, ROLL: %u", minimum, maximum, roll);

    ObjectGuid guid = GetPlayer()->GetGUID();

    WorldPacket data(SMSG_RANDOM_ROLL, 4 + 4 + 4 + 1 + 8);
    data << uint32(roll);
    data << uint32(minimum);
    data << uint32(maximum);

    data.WriteGuidMask(guid, 0, 6, 7, 1, 4, 5, 2, 3);

    data.WriteGuidBytes(guid, 5, 4, 2, 0, 3, 1, 6, 7);

    if (GetPlayer()->GetGroup())
        GetPlayer()->GetGroup()->BroadcastPacket(&data, false);
    else
        SendPacket(&data);
}

void WorldSession::HandleRaidTargetUpdateOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_RAID_TARGET_UPDATE");

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    uint8 x;
    uint8 iconID;
   
    recvData >> x;
    recvData >> iconID;

    /** error handling **/
    /********************/

    // everything's fine, do it
    if (x == 0xFF)                                           // target icon request
        group->SendTargetIconList(this);
    else                                                    // target icon update
    {
        if (!group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
            return;

        ObjectGuid guid;

        recvData.ReadGuidMask(guid, 3, 2, 1, 5, 0, 6, 7, 4);

        recvData.FlushBits();

        recvData.ReadGuidBytes(guid, 2, 3, 0, 7, 5, 1, 6, 4);

        group->SetTargetIcon(iconID, _player->GetGUID(), guid);
    }
}

void WorldSession::HandleGroupRaidConvertOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_RAID_CONVERT");

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (_player->InBattleground())
        return;

    // error handling
    if (!group->IsLeader(GetPlayer()->GetGUID()) || group->GetMembersCount() < 2)
        return;

    // everything's fine, do it (is it 0 (PARTY_OP_INVITE) correct code)
    SendPartyResult(PARTY_OP_INVITE, "", ERR_PARTY_RESULT_OK);

    // New 4.x: it is now possible to convert a raid to a group if member count is 5 or less

    bool toRaid;
    toRaid = recvData.ReadBit();

    if (toRaid)
        group->ConvertToRaid();
    else
        group->ConvertToGroup();
}

void WorldSession::HandleGroupChangeSubGroupOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_CHANGE_SUB_GROUP");

    // we will get correct pointer for group here, so we don't have to check if group is BG raid
    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    std::string name;
    uint8 groupNr;
    recvData >> name;
    recvData >> groupNr;

    if (groupNr >= MAX_RAID_SUBGROUPS)
        return;

    uint64 senderGuid = GetPlayer()->GetGUID();
    if (!group->IsLeader(senderGuid) && !group->IsAssistant(senderGuid))
        return;

    if (!group->HasFreeSlotSubGroup(groupNr))
        return;

    Player* movedPlayer = sObjectAccessor->FindPlayerByName(name);
    uint64 guid;

    if (movedPlayer)
        guid = movedPlayer->GetGUID();
    else
    {
        CharacterDatabase.EscapeString(name);
        guid = sObjectMgr->GetPlayerGUIDByName(name.c_str());
    }

    group->ChangeMembersGroup(guid, groupNr);
}

void WorldSession::HandleGroupSwapSubGroupOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_SWAP_SUB_GROUP");
    std::string unk1;
    std::string unk2;

    recvData >> unk1;
    recvData >> unk2;
}

void WorldSession::HandleGroupAssistantLeaderOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_GROUP_ASSISTANT_LEADER");

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (!group->IsLeader(GetPlayer()->GetGUID()))
        return;

    bool apply;

    ObjectGuid guid;

    recvData >> apply;
    recvData.ReadGuidMask(guid, 2, 0, 6, 3, 1);
    recvData.rfinish();
    recvData.ReadGuidMask(guid, 4, 5, 7);
    recvData.FlushBits();
    recvData.ReadGuidBytes(guid, 5, 1, 0, 7, 3, 6, 2, 4);

    group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_ASSISTANT);

    group->SendUpdate();
}

void WorldSession::HandlePartyAssignmentOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received MSG_PARTY_ASSIGNMENT");

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    uint64 senderGuid = GetPlayer()->GetGUID();
    if (!group->IsLeader(senderGuid) && !group->IsAssistant(senderGuid))
        return;

    uint8 assignment;
    bool apply;
    uint64 guid;
    recvData >> assignment >> apply;
    recvData >> guid;

    switch (assignment)
    {
        case GROUP_ASSIGN_MAINASSIST:
            group->RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINASSIST);
            group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_MAINASSIST);
            break;
        case GROUP_ASSIGN_MAINTANK:
            group->RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINTANK);           // Remove main assist flag from current if any.
            group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_MAINTANK);
        default:
            break;
    }

    group->SendUpdate();
}

void WorldSession::HandleRaidReadyCheckOpcode(WorldPacket& /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_RAID_READY_CHECK");

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    ObjectGuid playerGuid = GetPlayer()->GetGUID();

    /** error handling **/
    if (!group->IsLeader(playerGuid) && !group->IsAssistant(playerGuid))
        return;

    // check is also done client side
    if (group->ReadyCheckInProgress())
        return;
    /********************/

    uint32 readyCheckDuration = 35000;
    ObjectGuid groupGuid = group->GetGUID();

    // everything's fine, do it
    WorldPacket data(SMSG_RAID_READY_CHECK, 1 + 8 + 1 + 8 + 1 + 4);
    data.WriteGuidMask(groupGuid, 4, 2);
    data.WriteBit(playerGuid[4]);
    data.WriteGuidMask(groupGuid, 3, 7, 1, 0);
    data.WriteGuidMask(playerGuid, 6, 5);
    data.WriteGuidMask(groupGuid, 6, 5);
    data.WriteGuidMask(playerGuid, 0, 1, 2, 7, 3);

    data << uint32(readyCheckDuration);
    data.WriteGuidBytes(groupGuid, 2, 7, 3);
    data.WriteByteSeq(playerGuid[4]);
    data.WriteGuidBytes(groupGuid, 1, 0);
    data.WriteGuidBytes(playerGuid, 1, 2, 6, 5);
    data.WriteByteSeq(groupGuid[6]);
    data.WriteByteSeq(playerGuid[0]);
    data << uint8(0);                       // unknown
    data.WriteByteSeq(playerGuid[7]);
    data.WriteByteSeq(groupGuid[4]);
    data.WriteByteSeq(playerGuid[3]);
    data.WriteByteSeq(groupGuid[5]);

    group->BroadcastPacket(&data, false);

    group->ReadyCheck(true);
    group->ReadyCheckMemberHasResponded(playerGuid);
    group->OfflineReadyCheck();

    // leader keeps track of ready check timer
    GetPlayer()->SetReadyCheckTimer(readyCheckDuration);
}

void WorldSession::HandleRaidReadyCheckConfirmOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_RAID_READY_CHECK_CONFIRM");

    ObjectGuid guid;    // currently unused

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (!group->ReadyCheckInProgress())
        return;

    recvData.read_skip<uint8>();

    recvData.ReadGuidMask(guid, 2, 1, 0, 3, 6);
    bool status = recvData.ReadBit();
    recvData.ReadGuidMask(guid, 7, 4, 5);

    recvData.ReadGuidBytes(guid, 1, 0, 3, 2, 4, 5, 7, 6);

    ObjectGuid groupGuid = group->GetGUID();
    ObjectGuid playerGuid = GetPlayer()->GetGUID();

    WorldPacket data(SMSG_RAID_READY_CHECK_CONFIRM, 1 + 1 + 8 + 1 + 8);
    data.WriteBit(groupGuid[4]);
    data.WriteGuidMask(playerGuid, 5, 3);
    data.WriteBit(status);
    data.WriteBit(groupGuid[2]);
    data.WriteBit(playerGuid[6]);
    data.WriteBit(groupGuid[3]);
    data.WriteGuidMask(playerGuid, 0, 1);
    data.WriteGuidMask(groupGuid, 1, 5);
    data.WriteGuidMask(playerGuid, 7, 4);
    data.WriteBit(groupGuid[6]);
    data.WriteBit(playerGuid[2]);
    data.WriteGuidMask(groupGuid, 0, 7);
    data.FlushBits();

    data.WriteGuidBytes(playerGuid, 4, 2, 1);
    data.WriteGuidBytes(groupGuid, 4, 2);
    data.WriteByteSeq(playerGuid[0]);
    data.WriteGuidBytes(groupGuid, 5, 3);
    data.WriteByteSeq(playerGuid[7]);
    data.WriteGuidBytes(groupGuid, 6, 1);
    data.WriteGuidBytes(playerGuid, 6, 3, 5);
    data.WriteGuidBytes(groupGuid, 0, 7);

    group->BroadcastPacket(&data, false);
    group->ReadyCheckMemberHasResponded(playerGuid);

    if (group->ReadyCheckAllResponded())
    {
        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        if (leader)
            leader->SetReadyCheckTimer(0);

        group->ReadyCheck(false);
        group->ReadyCheckResetResponded();
        group->SendReadyCheckCompleted();
    }
}

void WorldSession::HandleRaidReadyCheckFinishedOpcode(WorldPacket& /*recvData*/)
{
    //Group* group = GetPlayer()->GetGroup();
    //if (!group)
    //    return;

    //if (!group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
    //    return;

    // Is any reaction need?
}

void WorldSession::BuildPartyMemberStatsChangedPacket(Player* player, WorldPacket* data)
{
    uint32 mask = player->GetGroupUpdateFlag();
    ObjectGuid guid = player->GetGUID();

    if (mask == GROUP_UPDATE_FLAG_NONE)
        return;

    std::set<uint32> phases;
    player->GetPhaseMgr().GetActivePhases(phases);

    if (mask & GROUP_UPDATE_FLAG_POWER_TYPE)                // if update power type, update current/max power also
        mask |= (GROUP_UPDATE_FLAG_CUR_POWER | GROUP_UPDATE_FLAG_MAX_POWER);

    if (mask & GROUP_UPDATE_FLAG_PET_POWER_TYPE)            // same for pets
        mask |= (GROUP_UPDATE_FLAG_PET_CUR_POWER | GROUP_UPDATE_FLAG_PET_MAX_POWER);

    data->Initialize(SMSG_PARTY_MEMBER_STATS, 80);          // average value
    data->WriteBit(guid[0]);
    data->WriteBit(guid[5]);
    data->WriteBit(1); // Ukn 
    data->WriteBit(guid[1]);
    data->WriteBit(guid[4]);
    data->WriteBit(1); // Ukn
    data->WriteBit(guid[6]);
    data->WriteBit(guid[2]);
    data->WriteBit(guid[7]);
    data->WriteBit(guid[3]);

    data->WriteByteSeq(guid[3]);
    data->WriteByteSeq(guid[2]);
    data->WriteByteSeq(guid[6]);
    data->WriteByteSeq(guid[7]);
    data->WriteByteSeq(guid[5]);
    *data << uint32(mask);

    /*
    if (mask & GROUP_UPDATE_FLAG_STATUS)
    {
        uint16 playerStatus = MEMBER_STATUS_ONLINE;
        if (player->IsPvP())
            playerStatus |= MEMBER_STATUS_PVP;

        if (!player->IsAlive())
        {
            if (player->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                playerStatus |= MEMBER_STATUS_GHOST;
            else
                playerStatus |= MEMBER_STATUS_DEAD;
        }

        if (player->HasByteFlag(UNIT_FIELD_SHAPESHIFT_FORM, 1, UNIT_BYTE2_FLAG_FFA_PVP))
            playerStatus |= MEMBER_STATUS_PVP_FFA;

        if (player->isAFK())
            playerStatus |= MEMBER_STATUS_AFK;

        if (player->isDND())
            playerStatus |= MEMBER_STATUS_DND;

        *data << uint16(playerStatus);
    }
    */
    data->WriteByteSeq(guid[1]);
    data->WriteByteSeq(guid[4]);
    data->WriteByteSeq(guid[0]);
    *data << uint32(0);

/* 
    if (mask & GROUP_UPDATE_FLAG_CUR_HP)
        *data << uint32(player->GetHealth());

    if (mask & GROUP_UPDATE_FLAG_MAX_HP)
        *data << uint32(player->GetMaxHealth());

    Powers powerType = player->getPowerType();
    if (mask & GROUP_UPDATE_FLAG_POWER_TYPE)
        *data << uint8(powerType);

    if (mask & GROUP_UPDATE_FLAG_CUR_POWER)
        *data << uint16(player->GetPower(powerType));

    if (mask & GROUP_UPDATE_FLAG_MAX_POWER)
        *data << uint16(player->GetMaxPower(powerType));

    if (mask & GROUP_UPDATE_FLAG_LEVEL)
        *data << uint16(player->getLevel());

    if (mask & GROUP_UPDATE_FLAG_ZONE)
        *data << uint16(player->GetZoneId());

    if (mask & GROUP_UPDATE_FLAG_UNK100)
        *data << uint16(0);

    if (mask & GROUP_UPDATE_FLAG_POSITION)
    {
        *data << uint16(player->GetPositionX());
        *data << uint16(player->GetPositionY());
        *data << uint16(player->GetPositionZ());
    }

    if (mask & GROUP_UPDATE_FLAG_AURAS)
    {
        *data << uint8(0);
        uint64 auramask = player->GetAuraUpdateMaskForRaid();
        *data << uint64(auramask);
        *data << uint32(MAX_AURAS); // count
        for (uint32 i = 0; i < MAX_AURAS; ++i)
        {
            if (auramask & (uint64(1) << i))
            {
                AuraApplication const* aurApp = player->GetVisibleAura(i);
                if (!aurApp)
                {
                    *data << uint32(0);
                    *data << uint16(0);
                    continue;
                }

                *data << uint32(aurApp->GetBase()->GetId());
                *data << uint16(aurApp->GetFlags());

                if (aurApp->GetFlags() & AFLAG_ANY_EFFECT_AMOUNT_SENT)
                {
                    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    {
                        if (AuraEffect const* eff = aurApp->GetBase()->GetEffect(i))
                            *data << int32(eff->GetAmount());
                        else
                            *data << int32(0);
                    }
                }
            }
        }
    }

    Pet* pet = player->GetPet();
    if (mask & GROUP_UPDATE_FLAG_PET_GUID)
    {
        if (pet)
            *data << uint64(pet->GetGUID());
        else
            *data << uint64(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_NAME)
    {
        if (pet)
            *data << pet->GetName();
        else
            *data << uint8(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_MODEL_ID)
    {
        if (pet)
            *data << uint16(pet->GetDisplayId());
        else
            *data << uint16(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_CUR_HP)
    {
        if (pet)
            *data << uint32(pet->GetHealth());
        else
            *data << uint32(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_MAX_HP)
    {
        if (pet)
            *data << uint32(pet->GetMaxHealth());
        else
            *data << uint32(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_POWER_TYPE)
    {
        if (pet)
            *data << uint8(pet->getPowerType());
        else
            *data << uint8(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_CUR_POWER)
    {
        if (pet)
            *data << uint16(pet->GetPower(pet->getPowerType()));
        else
            *data << uint16(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_MAX_POWER)
    {
        if (pet)
            *data << uint16(pet->GetMaxPower(pet->getPowerType()));
        else
            *data << uint16(0);
    }

    if (mask & GROUP_UPDATE_FLAG_PET_AURAS)
    {
        if (pet)
        {
            *data << uint8(0);
            uint64 auramask = pet->GetAuraUpdateMaskForRaid();
            *data << uint64(auramask);
            *data << uint32(MAX_AURAS); // count
            for (uint32 i = 0; i < MAX_AURAS; ++i)
            {
                if (auramask & (uint64(1) << i))
                {
                    AuraApplication const* aurApp = pet->GetVisibleAura(i);
                    if (!aurApp)
                    {
                        *data << uint32(0);
                        *data << uint16(0);
                        continue;
                    }

                    *data << uint32(aurApp->GetBase()->GetId());
                    *data << uint16(aurApp->GetFlags());

                    if (aurApp->GetFlags() & AFLAG_ANY_EFFECT_AMOUNT_SENT)
                    {
                        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                        {
                            if (AuraEffect const* eff = aurApp->GetBase()->GetEffect(i))
                                *data << int32(eff->GetAmount());
                            else
                                *data << int32(0);
                        }
                    }
                }
            }
        }
        else
        {
            *data << uint8(0);
            *data << uint64(0);
        }
    }

    if (mask & GROUP_UPDATE_FLAG_VEHICLE_SEAT)
    {
        if (Vehicle* veh = player->GetVehicle())
            *data << uint32(veh->GetVehicleInfo()->m_seatID[player->m_movementInfo.transport.seat]);
        else
            *data << uint32(0);

    }

    if (mask & GROUP_UPDATE_FLAG_PHASE)
    {
        *data << uint32(phases.empty() ? 8 : 0);
        *data << uint32(phases.size());
        for (std::set<uint32>::const_iterator itr = phases.begin(); itr != phases.end(); ++itr)
            *data << uint16(*itr);
    }
    */
}

/*this procedure handles clients CMSG_REQUEST_PARTY_MEMBER_STATS request*/
void WorldSession::HandleRequestPartyMemberStatsOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_REQUEST_PARTY_MEMBER_STATS");
    ObjectGuid guid;

    recvData.ReadBit(); // unk bit

    guid[7] = recvData.ReadBit();
    guid[4] = recvData.ReadBit();
    guid[0] = recvData.ReadBit();
    guid[1] = recvData.ReadBit();
    guid[3] = recvData.ReadBit();
    guid[6] = recvData.ReadBit();
    guid[2] = recvData.ReadBit();
    guid[5] = recvData.ReadBit();

    recvData.ReadGuidBytes(guid, 3, 6, 5, 2, 1, 4, 0, 7);

    Player* player = HashMapHolder<Player>::Find(guid);
    if (!player)
    {
        WorldPacket data(SMSG_PARTY_MEMBER_STATS_FULL, 3+4+2);
        data << uint8(0);                                   // only for SMSG_PARTY_MEMBER_STATS_FULL, probably arena/bg related
        data.appendPackGUID(guid);
        data << uint32(GROUP_UPDATE_FLAG_STATUS);
        data << uint16(MEMBER_STATUS_OFFLINE);
        SendPacket(&data);
        return;
    }

    Pet* pet = player->GetPet();
    Powers powerType = player->getPowerType();
    std::set<uint32> phases;
    player->GetPhaseMgr().GetActivePhases(phases);

    WorldPacket data(SMSG_PARTY_MEMBER_STATS_FULL, 4+2+2+2+1+2*6+8+1+8);
    data << uint8(0);                                       // only for SMSG_PARTY_MEMBER_STATS_FULL, probably arena/bg related
    data.append(player->GetPackGUID());

    uint32 updateFlags = GROUP_UPDATE_FLAG_STATUS | GROUP_UPDATE_FLAG_CUR_HP | GROUP_UPDATE_FLAG_MAX_HP
                      | GROUP_UPDATE_FLAG_CUR_POWER | GROUP_UPDATE_FLAG_MAX_POWER | GROUP_UPDATE_FLAG_LEVEL
                      | GROUP_UPDATE_FLAG_ZONE | GROUP_UPDATE_FLAG_POSITION | GROUP_UPDATE_FLAG_AURAS
                      | GROUP_UPDATE_FLAG_PET_NAME | GROUP_UPDATE_FLAG_PET_MODEL_ID | GROUP_UPDATE_FLAG_PET_AURAS;

    if (powerType != POWER_MANA)
        updateFlags |= GROUP_UPDATE_FLAG_POWER_TYPE;

    if (pet)
        updateFlags |= GROUP_UPDATE_FLAG_PET_GUID | GROUP_UPDATE_FLAG_PET_CUR_HP | GROUP_UPDATE_FLAG_PET_MAX_HP
                    | GROUP_UPDATE_FLAG_PET_POWER_TYPE | GROUP_UPDATE_FLAG_PET_CUR_POWER | GROUP_UPDATE_FLAG_PET_MAX_POWER;

    if (player->GetVehicle())
        updateFlags |= GROUP_UPDATE_FLAG_VEHICLE_SEAT;

    if (!phases.empty())
        updateFlags |= GROUP_UPDATE_FLAG_PHASE;

    uint16 playerStatus = MEMBER_STATUS_ONLINE;
    if (player->IsPvP())
        playerStatus |= MEMBER_STATUS_PVP;

    if (!player->IsAlive())
    {
        if (player->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            playerStatus |= MEMBER_STATUS_GHOST;
        else
            playerStatus |= MEMBER_STATUS_DEAD;
    }

    if (player->HasByteFlag(UNIT_FIELD_SHAPESHIFT_FORM, 1, UNIT_BYTE2_FLAG_FFA_PVP))
        playerStatus |= MEMBER_STATUS_PVP_FFA;

    if (player->isAFK())
        playerStatus |= MEMBER_STATUS_AFK;

    if (player->isDND())
        playerStatus |= MEMBER_STATUS_DND;

    data << uint32(updateFlags);
    data << uint16(playerStatus);                           // GROUP_UPDATE_FLAG_STATUS
    data << uint32(player->GetHealth());                    // GROUP_UPDATE_FLAG_CUR_HP
    data << uint32(player->GetMaxHealth());                 // GROUP_UPDATE_FLAG_MAX_HP
    if (updateFlags & GROUP_UPDATE_FLAG_POWER_TYPE)
        data << uint8(powerType);

    data << uint16(player->GetPower(powerType));            // GROUP_UPDATE_FLAG_CUR_POWER
    data << uint16(player->GetMaxPower(powerType));         // GROUP_UPDATE_FLAG_MAX_POWER
    data << uint16(player->getLevel());                     // GROUP_UPDATE_FLAG_LEVEL
    data << uint16(player->GetZoneId());                    // GROUP_UPDATE_FLAG_ZONE
    data << uint16(player->GetPositionX());                 // GROUP_UPDATE_FLAG_POSITION
    data << uint16(player->GetPositionY());                 // GROUP_UPDATE_FLAG_POSITION
    data << uint16(player->GetPositionZ());               // GROUP_UPDATE_FLAG_POSITION

    // GROUP_UPDATE_FLAG_AURAS
    data << uint8(1);
    uint64 auramask = 0;
    size_t maskPos = data.wpos();
    data << uint64(auramask);                          // placeholder
    data << uint32(MAX_AURAS);                         // count

    for (uint8 i = 0; i < MAX_AURAS; ++i)
    {
        if (AuraApplication const* aurApp = player->GetVisibleAura(i))
        {
            auramask |= (uint64(1) << i);

            data << uint32(aurApp->GetBase()->GetId());
            data << uint16(aurApp->GetFlags());

            if (aurApp->GetFlags() & AFLAG_ANY_EFFECT_AMOUNT_SENT)
            {
                for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                {
                    if (AuraEffect const* eff = aurApp->GetBase()->GetEffect(i))
                        data << int32(eff->GetAmount());
                    else
                        data << int32(0);
                }
            }
        }
    }

    data.put<uint64>(maskPos, auramask);                    // GROUP_UPDATE_FLAG_AURAS

    if (updateFlags & GROUP_UPDATE_FLAG_PET_GUID)
        data << uint64(pet->GetGUID());

    data << std::string(pet ? pet->GetName() : "");         // GROUP_UPDATE_FLAG_PET_NAME
    data << uint16(pet ? pet->GetDisplayId() : 0);          // GROUP_UPDATE_FLAG_PET_MODEL_ID

    if (updateFlags & GROUP_UPDATE_FLAG_PET_CUR_HP)
        data << uint32(pet->GetHealth());

    if (updateFlags & GROUP_UPDATE_FLAG_PET_MAX_HP)
        data << uint32(pet->GetMaxHealth());

    if (updateFlags & GROUP_UPDATE_FLAG_PET_POWER_TYPE)
        data << (uint8)pet->getPowerType();

    if (updateFlags & GROUP_UPDATE_FLAG_PET_CUR_POWER)
        data << uint16(pet->GetPower(pet->getPowerType()));

    if (updateFlags & GROUP_UPDATE_FLAG_PET_MAX_POWER)
        data << uint16(pet->GetMaxPower(pet->getPowerType()));

    // GROUP_UPDATE_FLAG_PET_AURAS
    uint64 petAuraMask = 0;
    data << uint8(1);
    maskPos = data.wpos();
    data << uint64(petAuraMask);                            // placeholder
    data << uint32(MAX_AURAS);                              // count
    if (pet)
    {
        for (uint8 i = 0; i < MAX_AURAS; ++i)
        {
            if (AuraApplication const* aurApp = pet->GetVisibleAura(i))
            {
                petAuraMask |= (uint64(1) << i);

                data << uint32(aurApp->GetBase()->GetId());
                data << uint16(aurApp->GetFlags());

                if (aurApp->GetFlags() & AFLAG_ANY_EFFECT_AMOUNT_SENT)
                {
                    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    {
                        if (AuraEffect const* eff = aurApp->GetBase()->GetEffect(i))
                            data << int32(eff->GetAmount());
                        else
                            data << int32(0);
                    }
                }
            }
        }
    }

    data.put<uint64>(maskPos, petAuraMask);                 // GROUP_UPDATE_FLAG_PET_AURAS

    if (updateFlags & GROUP_UPDATE_FLAG_VEHICLE_SEAT)
        data << uint32(player->GetVehicle()->GetVehicleInfo()->m_seatID[player->m_movementInfo.transport.seat]);

    if (updateFlags & GROUP_UPDATE_FLAG_PHASE)
    {
        data << uint32(phases.empty() ? 8 : 0);
        data << uint32(phases.size());
        for (std::set<uint32>::const_iterator itr = phases.begin(); itr != phases.end(); ++itr)
            data << uint16(*itr);
    }

    SendPacket(&data);
}

void WorldSession::HandleRequestRaidInfoOpcode(WorldPacket& /*recvData*/)
{
    // every time the player checks the character screen
    _player->SendRaidInfo();
}

void WorldSession::HandleOptOutOfLootOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_OPT_OUT_OF_LOOT");

    bool passOnLoot;
    recvData >> passOnLoot; // 1 always pass, 0 do not pass

    // ignore if player not loaded
    if (!GetPlayer())                                        // needed because STATUS_AUTHED
    {
        if (passOnLoot)
            TC_LOG_ERROR("network", "CMSG_OPT_OUT_OF_LOOT value<>0 for not-loaded character!");
        return;
    }

    GetPlayer()->SetPassOnGroupLoot(passOnLoot);
}

void WorldSession::HandleClearWorldMarkerOpcode(WorldPacket& recv_data)
{
    uint8 slot;
    recv_data >> slot;

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_CLEAR_WORLD_MARKER from %s (%u) slot: %u",
        GetPlayerName().c_str(), GetAccountId(), slot);

    Group* group = _player->GetGroup();
    if (!group)
        return;

    bool isEligibleDueToRaid = group->IsAssistant(_player->GetGUID()) || group->IsLeader(_player->GetGUID());
    if ((group->isRaidGroup() && isEligibleDueToRaid) || (!group->isRaidGroup()))
        group->ClearWorldMarker(slot);
}

void WorldSession::HandleSetEveryoneIsAssistant(WorldPacket& recv_data)
{
    bool apply = recv_data.ReadBit();

    TC_LOG_DEBUG("network", "WORLD: Received CMSG_SET_EVERYONE_IS_ASSISTANT from %s (%u) apply: %u",
        GetPlayerName().c_str(), GetAccountId(), apply ? 1 : 0);

    // Only raid groups may have assistant
    Group* group = _player->GetGroup();
    if (!group || !group->isRaidGroup()) 
        return;

    if (!group->IsLeader(_player->GetGUID()))
        return;

    for (Group::member_citerator itr = group->GetMemberSlots().begin(); itr != group->GetMemberSlots().end(); ++itr)
        group->SetGroupMemberFlag(itr->guid, true, MEMBER_FLAG_ASSISTANT);

    group->SendUpdate();
}
