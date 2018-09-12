// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "Utils/SchemaDatabase.h"

#include <improbable/c_worker.h>

#include "SpatialTypebindingManager.generated.h"

enum ERPCType
{
	RPC_Client = 0,
	RPC_Server,
	RPC_CrossServer,
	RPC_NetMulticast,
	RPC_Count
};

enum EReplicatedPropertyGroup : uint32
{
	GROUP_SingleClient,
	GROUP_MultiClient
};

FORCEINLINE EReplicatedPropertyGroup GetGroupFromCondition(ELifetimeCondition Condition)
{
	switch (Condition)
	{
	case COND_AutonomousOnly:
	case COND_OwnerOnly:
		return GROUP_SingleClient;
	default:
		return GROUP_MultiClient;
	}
}

struct FRPCInfo
{
	ERPCType Type;
	uint32 Index;
};

USTRUCT()
struct FClassInfo
{
	GENERATED_BODY()

	TMap<ERPCType, TArray<UFunction*>> RPCs;
	TMap<UFunction*, FRPCInfo> RPCInfoMap;

	Worker_ComponentId SingleClientComponent;
	Worker_ComponentId MultiClientComponent;
	Worker_ComponentId HandoverComponent;
	Worker_ComponentId RPCComponents[RPC_Count];
	Worker_ComponentId WorkingSetComponent;

	TSet<UClass*> SubobjectClasses;
};

UCLASS()
class SPATIALGDK_API USpatialTypebindingManager : public UObject
{
	GENERATED_BODY()

public:
	void Init();

	bool IsSupportedClass(UClass* Class);
	FClassInfo* FindClassInfoByClass(UClass* Class);
	FClassInfo* FindClassInfoByComponentId(Worker_ComponentId ComponentId);
	UClass* FindClassByComponentId(Worker_ComponentId ComponentId);

private:
	void FindSupportedClasses();
	void CreateTypebindings();

private:
	UPROPERTY()
	USchemaDatabase* SchemaDatabase;

	UPROPERTY()
	TArray<UClass*> SupportedClasses;

	UPROPERTY()
	TMap<UClass*, FClassInfo> ClassInfoMap;

	TMap<Worker_ComponentId, UClass*> ComponentToClassMap;
};
