/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
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

#ifndef _GROUPREFMANAGER
#define _GROUPREFMANAGER

#include "GroupReference.h"
#include "RefManager.h"

class Group;
class Player;

class GroupRefManager : public RefManager<Group, Player>
{
    public:
        GroupReference* getFirst() { return ((GroupReference*)RefManager<Group, Player>::getFirst()); }
        GroupReference const* getFirst() const { return ((GroupReference const*)RefManager<Group, Player>::getFirst()); }
};

//npcbot
class Creature;

class GroupBotRefManager : public RefManager<Group, Creature>
{
    public:
        GroupBotReference* getFirst() { return ((GroupBotReference*)RefManager<Group, Creature>::getFirst()); }
        GroupBotReference const* getFirst() const { return ((GroupBotReference const*)RefManager<Group, Creature>::getFirst()); }
};
//end npcbot
#endif
