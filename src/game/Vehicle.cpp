/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2011-2012 /dev/rsa for MangosR2 <http://github.com/MangosR2>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Vehicle.h"
#include "Unit.h"
#include "CreatureAI.h"
#include "Util.h"
#include "WorldPacket.h"
#include "movement/MoveSpline.h"
#include "SQLStorages.h"

VehicleKit::VehicleKit(Unit* base, VehicleEntry const* entry) 
    :  m_pBase(base), TransportBase(base), m_vehicleEntry(entry), m_uiNumFreeSeats(0), m_isInitialized(false)
{
    for (uint32 i = 0; i < MAX_VEHICLE_SEAT; ++i)
    {
        uint32 seatId = GetEntry()->m_seatID[i];

        if (!seatId)
            continue;


        if (VehicleSeatEntry const *seatInfo = sVehicleSeatStore.LookupEntry(seatId))
        {
            m_Seats.insert(std::make_pair(i, VehicleSeat(seatInfo)));

            if (seatInfo->IsUsable())
                ++m_uiNumFreeSeats;
        }
    }

    if (base)
    {
        if (GetEntry()->m_flags & VEHICLE_FLAG_NO_STRAFE)
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_NO_STRAFE);

        if (GetEntry()->m_flags & VEHICLE_FLAG_NO_JUMPING)
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_NO_JUMPING);

        if (GetEntry()->m_flags & VEHICLE_FLAG_FULLSPEEDTURNING)
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_FULLSPEEDTURNING);

        if (GetEntry()->m_flags & VEHICLE_FLAG_ALLOW_PITCHING)
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_ALLOW_PITCHING);

        if (GetEntry()->m_flags & VEHICLE_FLAG_FULLSPEEDPITCHING)
        {
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_ALLOW_PITCHING);
            GetBase()->m_movementInfo.AddMovementFlag2(MOVEFLAG2_FULLSPEEDPITCHING);
        }

    }
    SetDestination();
}

VehicleKit::~VehicleKit()
{
    Reset();
    GetBase()->RemoveSpellsCausingAura(SPELL_AURA_CONTROL_VEHICLE);
}

void VehicleKit::Initialize(uint32 creatureEntry)
{
    InstallAllAccessories(creatureEntry ? creatureEntry : m_pBase->GetEntry());
    UpdateFreeSeatCount();
    m_isInitialized = true;
}

void VehicleKit::RemoveAllPassengers()
{
    for (SeatMap::iterator itr = m_Seats.begin(); itr != m_Seats.end(); ++itr)
    {
        if (Unit* passenger = GetBase()->GetMap()->GetUnit(itr->second.passenger))
        {
            passenger->ExitVehicle();

            // remove creatures of player mounts
            if (passenger->GetTypeId() == TYPEID_UNIT)
                passenger->AddObjectToRemoveList();
        }
    }
}

/*
 * Checks a specific seat for empty
 * If seatId < 0, every seat check
 */
bool VehicleKit::HasEmptySeat(int8 seatId) const
{
    if (seatId < 0)
        return (GetNextEmptySeatWithFlag(0) != -1);

    SeatMap::const_iterator seat = m_Seats.find(seatId);
    // need add check on accessories-only seats...

    if (seat == m_Seats.end())
        return false;

    return !seat->second.passenger;
}

/*
 * return next free seat with a specific vehicleSeatFlag
 * -1 will returned if no free seat found
 */
int8 VehicleKit::GetNextEmptySeatWithFlag(int8 seatId, bool next /*= true*/, uint32 vehicleSeatFlag /*= 0 */) const
{

    if (m_Seats.empty() || seatId >= MAX_VEHICLE_SEAT)
        return -1;

    if (next)
    {
        for (SeatMap::const_iterator seat = m_Seats.begin(); seat != m_Seats.end(); ++seat)
            if ((seatId < 0 || seat->first >= seatId) && !seat->second.passenger && (!vehicleSeatFlag || (seat->second.seatInfo->m_flags & vehicleSeatFlag)))
                return seat->first;
    }
    else
    {
        for (SeatMap::const_reverse_iterator seat = m_Seats.rbegin(); seat != m_Seats.rend(); ++seat)
            if ((seatId < 0 || seat->first <= seatId) && !seat->second.passenger && (!vehicleSeatFlag || (seat->second.seatInfo->m_flags & vehicleSeatFlag)))
                return seat->first;
    }

    return -1;
}

Unit* VehicleKit::GetPassenger(int8 seatId) const
{
    SeatMap::const_iterator seat = m_Seats.find(seatId);

    if (seat == m_Seats.end())
        return NULL;

    return GetBase()->GetMap()->GetUnit(seat->second.passenger);
}

// Helper function to undo the turning of the vehicle to calculate a relative position of the passenger when boarding
void VehicleKit::CalculateBoardingPositionOf(float gx, float gy, float gz, float go, float &lx, float &ly, float &lz, float &lo)
{
    NormalizeRotatedPosition(gx - GetBase()->GetPositionX(), gy - GetBase()->GetPositionY(), lx, ly);

    lz = gz - GetBase()->GetPositionZ();
    lo = MapManager::NormalizeOrientation(go - GetBase()->GetOrientation());
}

void VehicleKit::CalculateSeatPositionOf(int8 seatId, float &x, float &y, float &z, float &o)
{
    SeatMap::iterator seat = m_Seats.find(seatId);

    if (seat == m_Seats.end())
        return;

    VehicleSeatEntry const* seatInfo = seat->second.seatInfo;

    x = seatInfo->m_attachmentOffsetX + m_dst_x;
    y = seatInfo->m_attachmentOffsetY + m_dst_y;
    z = seatInfo->m_attachmentOffsetZ + m_dst_z;
    o = seatInfo->m_passengerYaw      + m_dst_o;
}

bool VehicleKit::AddPassenger(Unit* passenger, int8 seatId)
{
    SeatMap::iterator seat;

    if (seatId < 0) // no specific seat requirement
    {
        for (seat = m_Seats.begin(); seat != m_Seats.end(); ++seat)
        {
            if (!seat->second.passenger && (seat->second.seatInfo->IsUsable() || (seat->second.seatInfo->m_flags & SEAT_FLAG_UNCONTROLLED)))
                break;
        }

        if (seat == m_Seats.end()) // no available seat
            return false;
    }
    else
    {
        seat = m_Seats.find(seatId);

        if (seat == m_Seats.end())
            return false;

        if (seat->second.passenger)
            return false;
    }

    VehicleSeatEntry const* seatInfo = seat->second.seatInfo;
    seat->second.passenger = passenger->GetObjectGuid();
    seat->second.b_dismount = true;

    if (!(seatInfo->m_flags & SEAT_FLAG_FREE_ACTION))
        passenger->addUnitState(UNIT_STAT_ON_VEHICLE);

    m_pBase->SetPhaseMask(passenger->GetPhaseMask(), true);

    // Calculate passengers local position
    float lx, ly, lz, lo;
    CalculateBoardingPositionOf(passenger->GetPositionX(), passenger->GetPositionY(), passenger->GetPositionZ(), passenger->GetOrientation(), lx, ly, lz, lo);

    BoardPassenger(passenger, lx, ly, lz, lo, seat->first);        // Use TransportBase to store the passenger

    passenger->m_movementInfo.ClearTransportData();
    passenger->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);
    passenger->m_movementInfo.SetTransportData(GetBase()->GetObjectGuid(), lx, ly, lz, lo, WorldTimer::getMSTime(), seat->first, seatInfo);

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        ((Player*)passenger)->SetViewPoint(m_pBase);
        passenger->SetRoot(true);
    }

    if (seat->second.IsProtectPassenger())
    {
        switch (m_pBase->GetEntry())
        {
            case 33651:                                     // VX 001
            case 33432:                                     // Leviathan MX
            case 33118:                                     // Ignis (Ulduar)
            case 30234:                                     // Nexus Lord's Hover Disk (Eye of Eternity, Malygos Encounter)
            case 30248:                                     // Scion's of Eternity Hover Disk (Eye of Eternity, Malygos Encounter)
                break;
            case 28817:
            default:
                passenger->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
                break;
        }
        passenger->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    }

    if (seatInfo->m_flags & SEAT_FLAG_CAN_CONTROL)
    {
        if (!(GetEntry()->m_flags & (VEHICLE_FLAG_ACCESSORY)))
        {
            m_pBase->StopMoving();
            m_pBase->GetMotionMaster()->Clear();
            m_pBase->CombatStop(true);
        }
        m_pBase->DeleteThreatList();
        m_pBase->getHostileRefManager().deleteReferences();
        m_pBase->SetCharmerGuid(passenger->GetObjectGuid());
        m_pBase->addUnitState(UNIT_STAT_CONTROLLED);

        passenger->SetCharm(m_pBase);

        if (m_pBase->HasAuraType(SPELL_AURA_FLY) || m_pBase->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED))
        {
            WorldPacket data;
            data.Initialize(SMSG_MOVE_SET_CAN_FLY, 12);
            data << m_pBase->GetPackGUID();
            data << (uint32)(0);
            m_pBase->SendMessageToSet(&data,false);
        }

        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            m_pBase->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);

            if (CharmInfo* charmInfo = m_pBase->InitCharmInfo(m_pBase))
            {
                charmInfo->SetState(CHARM_STATE_ACTION,ACTIONS_DISABLE);
                charmInfo->InitVehicleCreateSpells(seat->first);
            }

            Player* player = (Player*)passenger;
            player->SetMover(m_pBase);
            player->SetClientControl(m_pBase, 1);
            player->VehicleSpellInitialize();
        }

        if(!(((Creature*)m_pBase)->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_KEEP_AI))
            ((Creature*)m_pBase)->AIM_Initialize();

        if (m_pBase->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE))
        {
            m_pBase->SetRoot(true);
        }
        else if (passenger->IsWalking() && !GetBase()->IsWalking())
            ((Creature*)m_pBase)->SetWalk(true, true);
        else if (!passenger->IsWalking() && GetBase()->IsWalking())
            ((Creature*)m_pBase)->SetWalk(false, true);
    }
    else if ((seatInfo->m_flags & SEAT_FLAG_FREE_ACTION) || (seatInfo->m_flags & SEAT_FLAG_CAN_ATTACK))
    {
        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            Player* player = (Player*)passenger;
            player->SetClientControl(m_pBase, 0);
        }
    }

    // need correct, position not normalized currently
    passenger->GetMotionMaster()->MoveBoardVehicle(seatInfo->m_attachmentOffsetX,
        seatInfo->m_attachmentOffsetY,
        seatInfo->m_attachmentOffsetZ,
        seatInfo->m_passengerYaw,
        seatInfo->m_enterSpeed < M_NULL_F ? BASE_CHARGE_SPEED : seatInfo->m_enterSpeed,
        0.0f);

    UpdateFreeSeatCount();

    if (m_pBase->GetTypeId() == TYPEID_UNIT)
    {
        if (((Creature*)m_pBase)->AI())
            ((Creature*)m_pBase)->AI()->PassengerBoarded(passenger, seat->first, true);
    }
    if (passenger->GetTypeId() == TYPEID_UNIT)
    {
        if (((Creature*)passenger)->AI())
            ((Creature*)passenger)->AI()->EnteredVehicle(m_pBase, seat->first, true);
    }

    if (b_dstSet && (seatInfo->m_flagsB & VEHICLE_SEAT_FLAG_B_EJECTABLE_FORCED))
    {
        uint32 delay = seatInfo->m_exitMaxDuration * IN_MILLISECONDS;
        m_pBase->AddEvent(new PassengerEjectEvent(seatId,*m_pBase), delay);
        DEBUG_LOG("Vehicle::AddPassenger eject event for %s added, delay %u",passenger->GetObjectGuid().GetString().c_str(), delay);
    }

    DEBUG_LOG("VehicleKit::AddPassenger passenger %s boarded on %s, transport offset %f %f %f %f (parent - %s)",
            passenger->GetObjectGuid().GetString().c_str(),
            passenger->m_movementInfo.GetTransportGuid().GetString().c_str(),
            passenger->m_movementInfo.GetTransportPos()->x,
            passenger->m_movementInfo.GetTransportPos()->y,
            passenger->m_movementInfo.GetTransportPos()->z,
            passenger->m_movementInfo.GetTransportPos()->o,
            GetBase()->m_movementInfo.GetTransportGuid().IsEmpty() ? "<none>" : GetBase()->m_movementInfo.GetTransportGuid().GetString().c_str());

    return true;
}

void VehicleKit::RemovePassenger(Unit* passenger, bool dismount)
{
    SeatMap::iterator seat;

    for (seat = m_Seats.begin(); seat != m_Seats.end(); ++seat)
        if (seat->second.passenger == passenger->GetObjectGuid())
            break;

    if (seat == m_Seats.end())
        return;

    seat->second.passenger.Clear();
    passenger->clearUnitState(UNIT_STAT_ON_VEHICLE);

    UnBoardPassenger(passenger);                            // Use TransportBase to remove the passenger from storage list

    passenger->m_movementInfo.ClearTransportData();
    passenger->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);

    if (seat->second.IsProtectPassenger())
        if (passenger->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE))
            passenger->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);

    if (seat->second.seatInfo->m_flags & SEAT_FLAG_CAN_CONTROL)
    {

        passenger->SetCharm(NULL);
        passenger->RemoveSpellsCausingAura(SPELL_AURA_CONTROL_VEHICLE);

        m_pBase->SetCharmerGuid(ObjectGuid());
        m_pBase->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
        m_pBase->clearUnitState(UNIT_STAT_CONTROLLED);

        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            Player* player = (Player*)passenger;
            player->SetClientControl(m_pBase, 0);
            player->RemovePetActionBar();
        }

        if(!(((Creature*)m_pBase)->GetCreatureInfo()->flags_extra & CREATURE_FLAG_EXTRA_KEEP_AI))
            ((Creature*)m_pBase)->AIM_Initialize();
    }

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        Player* player = (Player*)passenger;
        player->SetViewPoint(NULL);

        passenger->SetRoot(false);

        player->SetMover(player);
        player->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ROOT);

        if ((m_pBase->HasAuraType(SPELL_AURA_FLY) || m_pBase->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED)) &&
            (!player->HasAuraType(SPELL_AURA_FLY) && !player->HasAuraType(SPELL_AURA_MOD_FLIGHT_SPEED)))
        {
            WorldPacket data;
            data.Initialize(SMSG_MOVE_UNSET_CAN_FLY, 12);
            data << player->GetPackGUID();
            data << (uint32)(0);
            m_pBase->SendMessageToSet(&data,false);
            player->m_movementInfo.RemoveMovementFlag(MOVEFLAG_FLYING);
            player->m_movementInfo.RemoveMovementFlag(MOVEFLAG_CAN_FLY);
        }
    }
    UpdateFreeSeatCount();

    if (m_pBase->GetTypeId() == TYPEID_UNIT)
    {
        if (((Creature*)m_pBase)->AI())
            ((Creature*)m_pBase)->AI()->PassengerBoarded(passenger, seat->first, false);
    }
    if (passenger->GetTypeId() == TYPEID_UNIT)
    {
        if (((Creature*)passenger)->AI())
            ((Creature*)passenger)->AI()->EnteredVehicle(m_pBase, seat->first, false);
    }
    if (dismount && seat->second.b_dismount)
    {
        Dismount(passenger, seat->second.seatInfo);
        // only for flyable vehicles
        if (m_pBase->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING))
            m_pBase->CastSpell(passenger, 45472, true);    // Parachute
    }
}

void VehicleKit::Reset()
{
    RemoveAllPassengers();
    UpdateFreeSeatCount();
    m_isInitialized = false;
}

void VehicleKit::InstallAllAccessories(uint32 entry)
{
    SQLMultiStorage::SQLMSIteratorBounds<VehicleAccessory> const& bounds = sVehicleAccessoryStorage.getBounds<VehicleAccessory>(entry);
    for (SQLMultiStorage::SQLMultiSIterator<VehicleAccessory> itr = bounds.first; itr != bounds.second; ++itr)
        InstallAccessory(*itr);
}

void VehicleKit::InstallAccessory(VehicleAccessory const* accessory)
{
    if (Unit* passenger = GetPassenger(accessory->seatId))
    {
        // already installed
        if (passenger->GetEntry() == accessory->passengerEntry)
            return;
        m_pBase->RemoveSpellsCausingAura(SPELL_AURA_CONTROL_VEHICLE, passenger->GetObjectGuid());
    }

    if (Creature* summoned = m_pBase->SummonCreature(accessory->passengerEntry,
        m_pBase->GetPositionX() + accessory->m_offsetX, m_pBase->GetPositionY() + accessory->m_offsetY, m_pBase->GetPositionZ() + accessory->m_offsetZ, m_pBase->GetOrientation() + accessory->m_offsetX,
        TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30000))
    {
        summoned->SetCreatorGuid(ObjectGuid());
        summoned->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
        int32 seatId = accessory->seatId + 1;
        SetDestination(accessory->m_offsetX,accessory->m_offsetY,accessory->m_offsetZ,accessory->m_offsetO,0.0f,0.0f);
        summoned->CastCustomSpell(m_pBase, SPELL_RIDE_VEHICLE_HARDCODED, &seatId, &seatId, NULL, true);

        SetDestination();
        if (summoned->GetVehicle())
            DEBUG_LOG("Vehicle::InstallAccessory %s accessory added, seat %i (real %i) of %s",summoned->GetObjectGuid().GetString().c_str(), accessory->seatId, GetSeatId(summoned), m_pBase->GetObjectGuid().GetString().c_str());
        else
        {
            sLog.outError("Vehicle::InstallAccessory cannot install %s to seat %u of %s",summoned->GetObjectGuid().GetString().c_str(), accessory->seatId, m_pBase->GetObjectGuid().GetString().c_str());
            summoned->ForcedDespawn();
        }
    }
    else
        sLog.outError("Vehicle::InstallAccessory cannot summon creature id %u (seat %u of %s)",accessory->passengerEntry, accessory->seatId,m_pBase->GetObjectGuid().GetString().c_str());
}

void VehicleKit::UpdateFreeSeatCount()
{
    m_uiNumFreeSeats = 0;

    for (SeatMap::const_iterator itr = m_Seats.begin(); itr != m_Seats.end(); ++itr)
    {
        if (!itr->second.passenger && itr->second.seatInfo->IsUsable())
            ++m_uiNumFreeSeats;
    }

    uint32 flag = m_pBase->GetTypeId() == TYPEID_PLAYER ? UNIT_NPC_FLAG_PLAYER_VEHICLE : UNIT_NPC_FLAG_SPELLCLICK;

    if (m_uiNumFreeSeats)
        m_pBase->SetFlag(UNIT_NPC_FLAGS, flag);
    else
        m_pBase->RemoveFlag(UNIT_NPC_FLAGS, flag);
}

VehicleSeatEntry const* VehicleKit::GetSeatInfo(Unit* passenger)
{
    if (m_Seats.empty())
        return NULL;

    for (SeatMap::const_iterator itr = m_Seats.begin(); itr != m_Seats.end(); ++itr)
    {
        ObjectGuid guid = itr->second.passenger;
        if (Unit* _passenger = GetBase()->GetMap()->GetUnit(guid))
            if (_passenger == passenger)
                return itr->second.seatInfo;
    }
    return NULL;
}

int8 VehicleKit::GetSeatId(Unit* passenger)
{
    for (SeatMap::iterator itr = m_Seats.begin(); itr != m_Seats.end(); ++itr)
    {
        if (Unit* _passenger = GetBase()->GetMap()->GetUnit(itr->second.passenger))
            if (_passenger == passenger)
                return itr->first;
    }
    return -1;
}

void VehicleKit::Dismount(Unit* passenger, VehicleSeatEntry const* seatInfo)
{
    if (!passenger || !passenger->IsInWorld() || !GetBase()->IsInWorld())
        return;

    float ox, oy, oz, oo;

    Unit* base = m_pBase->GetVehicle() ? m_pBase->GetVehicle()->GetBase() : m_pBase;
    base->GetPosition(ox, oy, oz);
    oo = base->GetOrientation();
//    passenger->Relocate(ox,oy,oz,oo);

    if (b_dstSet)
    {
        // parabolic traectory (catapults, explode, other effects). mostly set destination in DummyEffect.
        // destination Z not checked in this case! only limited on 8.0 delta. requred full correct set in spelleffects. 
        float speed = ((m_dst_speed > M_NULL_F) ? m_dst_speed : ((seatInfo && seatInfo->m_exitSpeed > M_NULL_F) ? seatInfo->m_exitSpeed : BASE_CHARGE_SPEED));
        float verticalSpeed = speed * sin(m_dst_elevation);
        float horisontalSpeed = speed * cos(m_dst_elevation);
        float moveTimeHalf =  verticalSpeed / ((seatInfo && seatInfo->m_exitGravity > 0.0f) ? seatInfo->m_exitGravity : Movement::gravity);
        float max_height = - Movement::computeFallElevation(moveTimeHalf,false,-verticalSpeed);

        passenger->GetMotionMaster()->MoveSkyDiving(m_dst_x,m_dst_y,m_dst_z,passenger->GetOrientation(), horisontalSpeed, max_height, true);
    }
    else if (seatInfo)
    {
        // half-parabolic traectory (unmount)
        float horisontalSpeed = seatInfo->m_exitSpeed;

        if (horisontalSpeed < M_NULL_F)
            horisontalSpeed = BASE_CHARGE_SPEED;

        // may be under water
        base->GetClosePoint(m_dst_x, m_dst_y, m_dst_z, base->GetObjectBoundingRadius(), frand(2.0f, 3.0f), frand(M_PI_F/2.0f,3.0f*M_PI_F/2.0f), passenger);
        if (m_dst_z < oz)
            m_dst_z = oz;

        passenger->GetMotionMaster()->MoveSkyDiving(m_dst_x, m_dst_y, m_dst_z + 0.1f, passenger->GetOrientation(), horisontalSpeed, 0.0f);
    }
    else
    {
        // jump from vehicle without seatInfo (? error case)
        base->GetClosePoint(m_dst_x, m_dst_y, m_dst_z, base->GetObjectBoundingRadius(), 2.0f, M_PI_F, passenger);
        passenger->UpdateAllowedPositionZ(m_dst_x, m_dst_y, m_dst_z);
        if (m_dst_z < oz)
            m_dst_z = oz;

        passenger->GetMotionMaster()->MoveSkyDiving(m_dst_x, m_dst_y, m_dst_z + 0.1f, passenger->GetOrientation(), BASE_CHARGE_SPEED, 0.0f);
    }

    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS,"VehicleKit::Dismount %s from %s (%f %f %f), destination point is %f %f %f",
        passenger->GetObjectGuid().GetString().c_str(),
        base->GetObjectGuid().GetString().c_str(),
        ox,oy,oz,
        m_dst_x,m_dst_y,m_dst_z);
    SetDestination();
}

void VehicleKit::SetDestination(float x, float y, float z, float o, float speed, float elevation)
{
    m_dst_x = x;
    m_dst_y = y;
    m_dst_z  = z;
    m_dst_o  = o;

    m_dst_speed  = speed;
    m_dst_elevation  = elevation;

    if (fabs(m_dst_x) > 0.001 ||
        fabs(m_dst_y) > 0.001 ||
        fabs(m_dst_z) > 0.001 ||
        fabs(m_dst_o) > 0.001 ||
        fabs(m_dst_speed) > 0.001 ||
        fabs(m_dst_elevation) > 0.001)
        b_dstSet = true;
};

bool VehicleSeat::IsProtectPassenger() const
{
    if (seatInfo &&
        ((seatInfo->m_flags & SEAT_FLAG_UNATTACKABLE) ||
        (seatInfo->m_flags &  SEAT_FLAG_HIDE_PASSENGER) ||
        (seatInfo->m_flags & SEAT_FLAG_CAN_CONTROL)) &&
        !(seatInfo->m_flags &  SEAT_FLAG_FREE_ACTION))
        return true;

    return false;
}

Aura* VehicleKit::GetControlAura(Unit* passenger)
{
    if (!passenger)
        return NULL;

    ObjectGuid casterGuid = passenger->GetObjectGuid();

    MAPLOCK_READ(GetBase(),MAP_LOCK_TYPE_AURAS);
    Unit::AuraList& auras = GetBase()->GetAurasByType(SPELL_AURA_CONTROL_VEHICLE);
    for (Unit::AuraList::iterator itr = auras.begin(); itr != auras.end(); ++itr)
    {
        if (!itr->IsEmpty() && (*itr)->GetCasterGuid() == casterGuid)
            return (*itr)();
    }
    return NULL;
}

void VehicleKit::DisableDismount(Unit* passenger)
{
    if (!passenger)
        return;

    int8 seatId = GetSeatId(passenger);

    if (seatId == -1)
        return;

    m_Seats[seatId].b_dismount = false;
}
