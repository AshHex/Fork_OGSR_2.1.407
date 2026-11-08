////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_communication_manager.h
//	Created 	: 03.09.2003
//  Modified 	: 14.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife communication manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_simulator_base.h"

class xrServer;
class CSE_ALifeSchedulable;
class CSE_ALifeHumanAbstract;
class CSE_ALifeTrader;
class CSE_ALifeTraderAbstract;

class CALifeCommunicationManager : public virtual CALifeSimulatorBase
{
protected:
    enum
    {
        MAX_STACK_DEPTH = 128,
        SUM_COUNT_THRESHOLD = 30,
    };

    ALife::ITEM_P_VECTOR m_tpItems1;
    ALife::ITEM_P_VECTOR m_tpItems2;
    ALife::OBJECT_VECTOR m_tpBlockedItems1;
    ALife::OBJECT_VECTOR m_tpBlockedItems2;

    ALife::ITEM_P_VECTOR m_tpTrader1;
    ALife::ITEM_P_VECTOR m_tpTrader2;
    ALife::INT_VECTOR m_tpSums1;
    ALife::INT_VECTOR m_tpSums2;

    ALife::SSumStackCell m_tpStack1[MAX_STACK_DEPTH];
    ALife::SSumStackCell m_tpStack2[MAX_STACK_DEPTH];

    u32 dwfComputeItemCost(ALife::ITEM_P_VECTOR& items);

    void vfRunFunctionByIndex(
        CSE_ALifeHumanAbstract* human,
        ALife::OBJECT_VECTOR& blocked_items,
        ALife::ITEM_P_VECTOR& items,
        int function_index,
        int& item_count);

    void vfAssignItemParents(CSE_ALifeHumanAbstract* human, int item_count);
    void vfAppendBlockedItems(
        CSE_ALifeHumanAbstract* human,
        ALife::OBJECT_VECTOR& blocked1,
        ALife::OBJECT_VECTOR& blocked2,
        int item_count);

    void vfAttachOwnerItems(
        CSE_ALifeHumanAbstract* human,
        ALife::ITEM_P_VECTOR& items,
        ALife::ITEM_P_VECTOR& own_items);

    void vfRestoreItems(CSE_ALifeHumanAbstract* human, ALife::ITEM_P_VECTOR& items);

    int ifComputeBalance(CSE_ALifeHumanAbstract* human, ALife::ITEM_P_VECTOR& items);

    void vfFillTraderVector(
        CSE_ALifeHumanAbstract* human,
        int item_count,
        ALife::ITEM_P_VECTOR& items);

    void vfGenerateSums(ALife::ITEM_P_VECTOR& trader, ALife::INT_VECTOR& sums);

    bool bfGetItemIndexes(
        ALife::ITEM_P_VECTOR& trader,
        int target_sum,
        ALife::INT_VECTOR& indexes,
        ALife::SSumStackCell* stack,
        int start_index,
        int stack_pointer);

    bool bfCheckForInventoryCapacity(
        CSE_ALifeHumanAbstract* human1,
        ALife::ITEM_P_VECTOR& trader1,
        ALife::INT_VECTOR& indexes1,
        CSE_ALifeHumanAbstract* human2,
        ALife::ITEM_P_VECTOR& trader2,
        ALife::INT_VECTOR& indexes2);

    bool bfCheckForInventoryCapacity(
        CSE_ALifeHumanAbstract* human1,
        ALife::ITEM_P_VECTOR& trader1,
        int sum1,
        int money1,
        CSE_ALifeHumanAbstract* human2,
        ALife::ITEM_P_VECTOR& trader2,
        int sum2,
        int money2,
        int balance);

    bool bfCheckForTrade(
        CSE_ALifeHumanAbstract* human1,
        ALife::ITEM_P_VECTOR& trader1,
        ALife::INT_VECTOR& sums1,
        int money1,
        CSE_ALifeHumanAbstract* human2,
        ALife::ITEM_P_VECTOR& trader2,
        ALife::INT_VECTOR& sums2,
        int money2,
        int balance);

    bool bfCheckIfCanNullTradersBalance(
        CSE_ALifeHumanAbstract* human1,
        CSE_ALifeHumanAbstract* human2,
        int item_count1,
        int item_count2,
        int balance);

    void vfPerformTrading(CSE_ALifeHumanAbstract* human1, CSE_ALifeHumanAbstract* human2);
    void vfPerformCommunication();

    void vfAttachGatheredItems(
        CSE_ALifeTraderAbstract* trader1,
        CSE_ALifeTraderAbstract* trader2,
        ALife::OBJECT_VECTOR& object_vector);

#ifdef DEBUG
    void vfPrintItems(CSE_ALifeHumanAbstract* human, ALife::ITEM_P_VECTOR& items);
    void vfPrintItems(CSE_ALifeHumanAbstract* human);
#endif

public:
    CALifeCommunicationManager(xrServer* server, LPCSTR section);

    void communicate_with_customer(
        CSE_ALifeHumanAbstract* human,
        CSE_ALifeTrader* trader);
};
