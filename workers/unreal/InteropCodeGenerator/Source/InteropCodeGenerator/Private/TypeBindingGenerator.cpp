// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "TypeBindingGenerator.h"
#include "TypeStructure.h"
#include "SchemaGenerator.h"

#include "Utils/CodeWriter.h"

// Needed for std::bind.
#include <functional>

FString TypeBindingName(UClass* Class)
{
	return FString::Printf(TEXT("USpatialTypeBinding_%s"), *Class->GetName());
}

void GenerateUnrealToSchemaConversion(FCodeWriter& Writer, const FString& Update, TArray<UProperty*> PropertyChain, const FString& PropertyValue, const bool bIsUpdate, TFunction<void(const FString&)> ObjectResolveFailureGenerator)
{
	// Get result type.
	UProperty* Property = PropertyChain[PropertyChain.Num() - 1];
	FString SpatialValueSetter = Update + TEXT(".set_") + SchemaFieldName(PropertyChain);

	if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
	{
		Writer.Printf("// UNSUPPORTED UEnumProperty %s(%s);", *SpatialValueSetter, *PropertyValue);
		//Writer.Print(FString::Printf(TEXT("auto Underlying = %s.GetValue()"), *PropertyValue));
		//return GenerateUnrealToSchemaConversion(Writer, EnumProperty->GetUnderlyingProperty(), TEXT("Underlying"), ResultName, Handle);
	}

	// Try to special case to custom types we know about
	if (Property->IsA(UStructProperty::StaticClass()))
	{
		UStructProperty * StructProp = Cast<UStructProperty>(Property);
		UScriptStruct * Struct = StructProp->Struct;
		if (Struct->GetFName() == NAME_Vector ||
			Struct->GetName() == TEXT("Vector_NetQuantize100") ||
			Struct->GetName() == TEXT("Vector_NetQuantize10") ||
			Struct->GetName() == TEXT("Vector_NetQuantizeNormal") ||
			Struct->GetName() == TEXT("Vector_NetQuantize"))
		{
			Writer.Printf("%s(improbable::Vector3f(%s.X, %s.Y, %s.Z));", *SpatialValueSetter, *PropertyValue, *PropertyValue, *PropertyValue);
		}
		else if (Struct->GetFName() == NAME_Rotator)
		{
			Writer.Printf("%s(improbable::unreal::UnrealFRotator(%s.Yaw, %s.Pitch, %s.Roll));", *SpatialValueSetter, *PropertyValue, *PropertyValue, *PropertyValue);
		}
		else if (Struct->GetFName() == NAME_Plane)
		{
			Writer.Printf("%s(improbable::unreal::UnrealFPlane(%s.X, %s.Y, %s.Z, %s.W));", *SpatialValueSetter, *PropertyValue, *PropertyValue, *PropertyValue, *PropertyValue);
		}
		else if (Struct->GetName() == TEXT("RepMovement") ||
			Struct->GetName() == TEXT("UniqueNetIdRepl"))
		{
			Writer.Print("{").Indent();
			Writer.Printf(R"""(
				TArray<uint8> ValueData;
				FMemoryWriter ValueDataWriter(ValueData);
				bool Success;
				%s.NetSerialize(ValueDataWriter, PackageMap, Success);
				%s(std::string(reinterpret_cast<char*>(ValueData.GetData()), ValueData.Num()));)""", *PropertyValue, *SpatialValueSetter);
			Writer.Outdent().Print("}");
		}
		else
		{
			// If this is not a struct that we handle explicitly, instead recurse in to its properties
			for (TFieldIterator<UProperty> It(Struct); It; ++It)
			{
				TArray<UProperty*> NewChain = PropertyChain;
				NewChain.Add(*It);
				GenerateUnrealToSchemaConversion(Writer, Update, NewChain, PropertyValue + TEXT(".") + (*It)->GetNameCPP(), bIsUpdate, ObjectResolveFailureGenerator);
			}
		}
	}
	else if (Property->IsA(UBoolProperty::StaticClass()))
	{
		Writer.Printf("%s(%s);", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UFloatProperty::StaticClass()))
	{
		Writer.Printf("%s(%s);", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UIntProperty::StaticClass()))
	{
		Writer.Printf("%s(%s);", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UByteProperty::StaticClass()))
	{
		Writer.Printf("%s(uint32_t(%s));", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UClassProperty::StaticClass()))
	{
		// todo David: UClasses are yet to be implemented. 
		// this is above UObjectProperty to make sure it isn't caught there.
		Writer.Printf("// UNSUPPORTED UClassProperty %s(%s);", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UObjectPropertyBase::StaticClass()))
	{
		Writer.Printf("if (%s != nullptr)", *PropertyValue);
		Writer.Print("{").Indent();
		Writer.Printf(R"""(
			FNetworkGUID NetGUID = PackageMap->GetNetGUIDFromObject(%s);
			improbable::unreal::UnrealObjectRef ObjectRef = PackageMap->GetUnrealObjectRefFromNetGUID(NetGUID);
			if (ObjectRef == SpatialConstants::UNRESOLVED_OBJECT_REF))""", *PropertyValue);
		Writer.Print("{").Indent();
		ObjectResolveFailureGenerator(*PropertyValue);
		Writer.Outdent().Print("}");
		Writer.Printf(R"""(
			else
			{
				%s(ObjectRef);
			})""", *SpatialValueSetter);
		Writer.Outdent().Print("}");
		Writer.Printf(R"""(
			else
			{
				%s(SpatialConstants::NULL_OBJECT_REF);
			})""", *SpatialValueSetter);
	}
	else if (Property->IsA(UNameProperty::StaticClass()))
	{
		Writer.Printf("%s(TCHAR_TO_UTF8(*%s.ToString()));", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UUInt32Property::StaticClass()))
	{
		Writer.Printf("%s(uint32_t(%s));", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UUInt64Property::StaticClass()))
	{
		Writer.Printf("%s(uint64_t(%s));", *SpatialValueSetter, *PropertyValue);
	}
	else if (Property->IsA(UStrProperty::StaticClass()))
	{
		Writer.Printf("%s(TCHAR_TO_UTF8(*%s));", *SpatialValueSetter, *PropertyValue);
	}
	else
	{
		Writer.Printf("// UNSUPPORTED U%s (unhandled) %s(%s)", *Property->GetClass()->GetName(), *SpatialValueSetter, *PropertyValue);
	}
}

void GeneratePropertyToUnrealConversion(FCodeWriter& Writer, const FString& Update, TArray<UProperty*> PropertyChain, const FString& PropertyValue, const bool bIsUpdate, const FString& PropertyType, TFunction<void(const FString&)> ObjectResolveFailureGenerator)
{
	FString SpatialValue;

	// This bool is used to differentiate between property updates and command arguments. Unlike command arguments, all property updates are optionals and must be accessed through .data()
	if (bIsUpdate)
	{
		SpatialValue = FString::Printf(TEXT("(*%s.%s().data())"), *Update, *SchemaFieldName(PropertyChain));
	}
	else
	{
		SpatialValue = FString::Printf(TEXT("%s.%s()"), *Update, *SchemaFieldName(PropertyChain));
	}

	// Get result type.
	UProperty* Property = PropertyChain[PropertyChain.Num() - 1];
	if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
	{
		Writer.Printf("// UNSUPPORTED UEnumProperty %s %s", *PropertyValue, *SpatialValue);
	}

	// Try to special case to custom types we know about
	if (Property->IsA(UStructProperty::StaticClass()))
	{
		UStructProperty * StructProp = Cast<UStructProperty>(Property);
		UScriptStruct * Struct = StructProp->Struct;
		if (Struct->GetFName() == NAME_Vector ||
			Struct->GetName() == TEXT("Vector_NetQuantize100") ||
			Struct->GetName() == TEXT("Vector_NetQuantize10") ||
			Struct->GetName() == TEXT("Vector_NetQuantizeNormal") ||
			Struct->GetName() == TEXT("Vector_NetQuantize"))
		{
			Writer.Print("{").Indent();
			Writer.Printf("auto& Vector = %s;", *SpatialValue);
			Writer.Printf("%s.X = Vector.x();", *PropertyValue);
			Writer.Printf("%s.Y = Vector.y();", *PropertyValue);
			Writer.Printf("%s.Z = Vector.z();", *PropertyValue);
			Writer.Outdent().Print("}");
		}
		else if (Struct->GetFName() == NAME_Rotator)
		{
			Writer.Print("{").Indent();
			Writer.Printf("auto& Rotator = %s;", *SpatialValue);
			Writer.Printf("%s.Yaw = Rotator.yaw();", *PropertyValue);
			Writer.Printf("%s.Pitch = Rotator.pitch();", *PropertyValue);
			Writer.Printf("%s.Roll = Rotator.roll();", *PropertyValue);
			Writer.Outdent().Print("}");
		}
		else if (Struct->GetFName() == NAME_Plane)
		{
			Writer.Print("{").Indent();
			Writer.Printf("auto& Plane = %s;", *SpatialValue);
			Writer.Printf("%s.X = Plane.x();", *PropertyValue);
			Writer.Printf("%s.Y = Plane.y();", *PropertyValue);
			Writer.Printf("%s.Z = Plane.z();", *PropertyValue);
			Writer.Printf("%s.W = Plane.w();", *PropertyValue);
			Writer.Outdent().Print("}");
		}
		else if (Struct->GetName() == TEXT("RepMovement") ||
			Struct->GetName() == TEXT("UniqueNetIdRepl"))
		{
			Writer.Print("{").Indent();
			Writer.Print(FString::Printf(TEXT(R"""(
				auto& ValueDataStr = %s;
				TArray<uint8> ValueData;
				ValueData.Append(reinterpret_cast<const uint8*>(ValueDataStr.data()), ValueDataStr.size());
				FMemoryReader ValueDataReader(ValueData);
				bool bSuccess;
				%s.NetSerialize(ValueDataReader, PackageMap, bSuccess);)"""), *SpatialValue, *PropertyValue));
			Writer.Outdent().Print("}");
		}
		else
		{
			for (TFieldIterator<UProperty> It(Struct); It; ++It)
			{
				TArray<UProperty*> NewChain = PropertyChain;
				NewChain.Add(*It);
				GeneratePropertyToUnrealConversion(Writer, Update, NewChain, PropertyValue + TEXT(".") + (*It)->GetNameCPP(), bIsUpdate, (*It)->GetCPPType(), ObjectResolveFailureGenerator);
			}
		}
	}
	else if (Property->IsA(UBoolProperty::StaticClass()))
	{
		Writer.Printf("%s = %s;", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UFloatProperty::StaticClass()))
	{
		Writer.Printf("%s = %s;", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UIntProperty::StaticClass()))
	{
		Writer.Printf("%s = %s;", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UByteProperty::StaticClass()))
	{
		// Byte properties are weird, because they can also be an enum in the form TEnumAsByte<...>. Therefore, the code generator needs to cast to either
		// TEnumAsByte<...> or uint8. However, as TEnumAsByte<...> only has a uint8 constructor, we need to cast the SpatialOS value into uint8 first
		// which causes "uint8(uint8(...))" to be generated for non enum bytes.
		Writer.Printf("%s = %s(uint8(%s));", *PropertyValue, *PropertyType, *SpatialValue);
	}
	else if (Property->IsA(UClassProperty::StaticClass()))
	{
		// todo David: UClasses are yet to be implemented. 
		// this is above UObjectProperty to make sure it isn't caught there.
		Writer.Printf("// UNSUPPORTED UClassProperty %s %s", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UObjectPropertyBase::StaticClass()))
	{
		Writer.Print("{").Indent();
		Writer.Printf(R"""(
			improbable::unreal::UnrealObjectRef ObjectRef = %s;
			check(ObjectRef != SpatialConstants::UNRESOLVED_OBJECT_REF);
			if (ObjectRef == SpatialConstants::NULL_OBJECT_REF)
			{
				%s = nullptr;
			})""", *SpatialValue, *PropertyValue);
		Writer.Print("else");
		Writer.Print("{").Indent();
		Writer.Printf(R"""(
			FNetworkGUID NetGUID = PackageMap->GetNetGUIDFromUnrealObjectRef(ObjectRef);
			if (NetGUID.IsValid())
			{
				UObject* Object_Raw = PackageMap->GetObjectFromNetGUID(NetGUID, true);
				checkf(Object_Raw, TEXT("An object ref %%s should map to a valid object."), *ObjectRefToString(ObjectRef));
				%s = dynamic_cast<%s>(Object_Raw);
				checkf(%s, TEXT("Object ref %%s maps to object %%s with the wrong class."), *ObjectRefToString(ObjectRef), *Object_Raw->GetFullName());
			})""", *PropertyValue, *PropertyType, *PropertyValue);
		Writer.Print("else");
		Writer.Print("{").Indent();
		ObjectResolveFailureGenerator(*PropertyValue);
		Writer.Outdent().Print("}");
		Writer.Outdent().Print("}");
		Writer.Outdent().Print("}");
	}
	else if (Property->IsA(UNameProperty::StaticClass()))
	{
		Writer.Printf("%s = FName((%s).data());", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UUInt32Property::StaticClass()))
	{
		Writer.Printf("%s = uint32(%s);", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UUInt64Property::StaticClass()))
	{
		Writer.Printf("%s = uint64(%s);", *PropertyValue, *SpatialValue);
	}
	else if (Property->IsA(UStrProperty::StaticClass()))
	{
		Writer.Printf("%s = FString(UTF8_TO_TCHAR(%s.c_str()));", *PropertyValue, *SpatialValue);
	}
	else
	{
		Writer.Printf("// UNSUPPORTED U%s (unhandled) %s %s", *Property->GetClass()->GetName(), *PropertyValue, *SpatialValue);
	}
}

FString GeneratePropertyReader(UProperty* Property)
{
	// This generates the appropriate macro for Unreal to read values from an FFrame. This is the same method as Unreal uses in .generated.h files
	if (Property->IsA(UBoolProperty::StaticClass()))
	{
		return FString::Printf(TEXT("P_GET_UBOOL(%s);"), *Property->GetName());
	}
	else if (Property->IsA(UObjectProperty::StaticClass()))
	{
		return FString::Printf(TEXT("P_GET_OBJECT(%s, %s);"),
			*GetFullCPPName(Cast<UObjectProperty>(Property)->PropertyClass),
			*Property->GetName());
	}
	else if (Property->IsA(UStructProperty::StaticClass()))
	{
		return FString::Printf(TEXT("P_GET_STRUCT(%s, %s)"),
			*Cast<UStructProperty>(Property)->Struct->GetStructCPPName(),
			*Property->GetName());
	}
	return FString::Printf(TEXT("P_GET_PROPERTY(%s, %s);"),
		*GetFullCPPName(Property->GetClass()),
		*Property->GetName());
}

void GenerateTypeBindingHeader(FCodeWriter& HeaderWriter, FString SchemaFilename, FString InteropFilename, UClass* Class, const FPropertyLayout& Layout)
{
	HeaderWriter.Printf(R"""(
		// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
		// Note that this file has been generated automatically
		#pragma once

		#include <improbable/worker.h>
		#include <improbable/view.h>
		#include <improbable/unreal/core_types.h>
		#include <improbable/unreal/unreal_metadata.h>
		#include <improbable/unreal/generated/%s.h>
		#include "ScopedViewCallbacks.h"
		#include "../SpatialTypeBinding.h"
		#include "SpatialTypeBinding_%s.generated.h")""", *SchemaFilename, *Class->GetName());
	HeaderWriter.Print();

	// Type binding class.
	HeaderWriter.Print("UCLASS()");
	HeaderWriter.Printf("class %s : public USpatialTypeBinding", *TypeBindingName(Class));
	HeaderWriter.Print("{").Indent();
	HeaderWriter.Print("GENERATED_BODY()");
	HeaderWriter.Print();
	HeaderWriter.Outdent().Print("public:").Indent();
	HeaderWriter.Print(R"""(
		static const FRepHandlePropertyMap& GetHandlePropertyMap();

		UClass* GetBoundClass() const override;

		void Init(USpatialInterop* InInterop, USpatialPackageMapClient* InPackageMap) override;
		void BindToView() override;
		void UnbindFromView() override;
		worker::ComponentId GetReplicatedGroupComponentId(EReplicatedPropertyGroup Group) const override;

		worker::Entity CreateActorEntity(const FString& ClientWorkerId, const FVector& Position, const FString& Metadata, const FPropertyChangeState& InitialChanges, USpatialActorChannel* Channel) const override;
		void SendComponentUpdates(const FPropertyChangeState& Changes, USpatialActorChannel* Channel, const FEntityId& EntityId) const override;
		void SendRPCCommand(UObject* TargetObject, const UFunction* const Function, FFrame* const Frame) override;

		void ReceiveAddComponent(USpatialActorChannel* Channel, UAddComponentOpWrapperBase* AddComponentOp) const override;
		void ApplyQueuedStateToChannel(USpatialActorChannel* ActorChannel) override;)""");
	HeaderWriter.Print();
	HeaderWriter.Outdent().Print("private:").Indent();
	HeaderWriter.Print("improbable::unreal::callbacks::FScopedViewCallbacks ViewCallbacks;");
	HeaderWriter.Print();
	HeaderWriter.Print("// Pending updates.");
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		HeaderWriter.Printf("TMap<FEntityId, improbable::unreal::%s::Data> Pending%sData;",
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group));
	}
	HeaderWriter.Print();
	HeaderWriter.Printf(R"""(
		// RPC to sender map.
		using FRPCSender = void (%s::*)(worker::Connection* const, struct FFrame* const, UObject*);
		TMap<FName, FRPCSender> RPCToSenderMap;)""", *TypeBindingName(Class));
	HeaderWriter.Print();

	HeaderWriter.Print("// Component update helper functions.");
	FFunctionSignature BuildComponentUpdateSignature;
	BuildComponentUpdateSignature.Type = "void";
	BuildComponentUpdateSignature.NameAndParams = "BuildSpatialComponentUpdate(\n\tconst FPropertyChangeState& Changes,\n\tUSpatialActorChannel* Channel";
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		BuildComponentUpdateSignature.NameAndParams += FString::Printf(TEXT(",\n\timprobable::unreal::%s::Update& %sUpdate,\n\tbool& b%sUpdateChanged"),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group));
	}
	BuildComponentUpdateSignature.NameAndParams += ") const";
	HeaderWriter.Print(BuildComponentUpdateSignature.Declaration());
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		HeaderWriter.Printf("void ServerSendUpdate_%s(const uint8* RESTRICT Data, int32 Handle, UProperty* Property, USpatialActorChannel* Channel, improbable::unreal::%s::Update& OutUpdate) const;",
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class));
	}
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		HeaderWriter.Printf("void ReceiveUpdate_%s(USpatialActorChannel* ActorChannel, const improbable::unreal::%s::Update& Update) const;",
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class));
	}

	HeaderWriter.Print();
	HeaderWriter.Print("// RPC command sender functions.");
	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			HeaderWriter.Printf("void %s_SendCommand(worker::Connection* const Connection, struct FFrame* const RPCFrame, UObject* TargetObject);",
				*RPC.Function->GetName());
		}
	}

	HeaderWriter.Print();
	HeaderWriter.Print("// RPC command request handler functions.");
	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			HeaderWriter.Printf("void %s_OnCommandRequest(const worker::CommandRequestOp<improbable::unreal::%s::Commands::%s>& Op);",
				*RPC.Function->GetName(),
				*SchemaRPCComponentName(Group, Class),
				*CPPCommandClassName(RPC.Function));
		}
	}

	HeaderWriter.Print();
	HeaderWriter.Print("// RPC command response handler functions.");
	for (auto Group : GetRPCTypes())
	{
		// Command response receiver function signatures
		for (auto& RPC : Layout.RPCs[Group])
		{
			HeaderWriter.Printf("void %s_OnCommandResponse(const worker::CommandResponseOp<improbable::unreal::%s::Commands::%s>& Op);",
				*RPC.Function->GetName(),
				*SchemaRPCComponentName(Group, Class),
				*CPPCommandClassName(RPC.Function));
		}
	}
	HeaderWriter.Outdent();
	HeaderWriter.Print("};");
}

void GenerateTypeBindingSource(FCodeWriter& SourceWriter, FString SchemaFilename, FString InteropFilename, UClass* Class, const FPropertyLayout& Layout)
{
	SourceWriter.Printf(R"""(
		// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
		// Note that this file has been generated automatically

		#include "%s.h"
		#include "Engine.h"

		#include "SpatialOS.h"
		#include "EntityBuilder.h"

		#include "../SpatialConstants.h"
		#include "../SpatialConditionMapFilter.h"
		#include "../SpatialUnrealObjectRef.h"
		#include "../SpatialActorChannel.h"
		#include "../SpatialPackageMapClient.h"
		#include "../SpatialNetDriver.h"
		#include "../SpatialInterop.h")""", *InteropFilename);
	// TODO: Come up with a generic solution to include the right headers.
	if (Class->GetName().Contains("WheeledVehicle"))
	{
		SourceWriter.Printf(R"""(
		#include "WheeledVehicle.h"
		#include "WheeledVehicleMovementComponent.h")""");
	}
	SourceWriter.Print();
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf("#include \"%sAddComponentOp.h\"", *SchemaReplicatedDataName(Group, Class));
	}

	// Generate methods implementations

	SourceWriter.Print();
	GenerateFunction_GetHandlePropertyMap(SourceWriter, Class, Layout);

	SourceWriter.Print();
	GenerateFunction_GetBoundClass(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_Init(SourceWriter, Class, Layout);

	SourceWriter.Print();
	GenerateFunction_BindToView(SourceWriter, Class, Layout);

	SourceWriter.Print();
	GenerateFunction_UnbindFromView(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_GetReplicatedGroupComponentId(SourceWriter, Class, Layout);

	SourceWriter.Print();
	GenerateFunction_CreateActorEntity(SourceWriter, Class, Layout);

	SourceWriter.Print();
	GenerateFunction_SendComponentUpdates(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_SendRPCCommand(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_ReceiveAddComponent(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_ApplyQueuedStateToChannel(SourceWriter, Class);

	SourceWriter.Print();
	GenerateFunction_BuildSpatialComponentUpdate(SourceWriter, Class);

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Print();
		GenerateFunction_ServerSendUpdate(SourceWriter, Class, Layout, Group);
	}

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Print();
		GenerateFunction_ReceiveUpdate(SourceWriter, Class, Layout, Group);
	}

	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			SourceWriter.Print();
			GenerateFunction_RPCSendCommand(SourceWriter, Class, RPC);
		}
	}

	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			SourceWriter.Print();
			GenerateFunction_RPCOnCommandRequest(SourceWriter, Class, RPC);
		}
	}

	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			SourceWriter.Print();
			GenerateFunction_RPCOnCommandResponse(SourceWriter, Class, RPC);
		}
	}
}

void GenerateFunction_GetHandlePropertyMap(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout)
{
	FFunctionWriter Func(SourceWriter, {"const FRepHandlePropertyMap&", "GetHandlePropertyMap()"}, TypeBindingName(Class));

	SourceWriter.Print("static FRepHandlePropertyMap HandleToPropertyMap;");
	SourceWriter.Print("if (HandleToPropertyMap.Num() == 0)");
	{
		FScopeWriter PropertyMapInitScope(SourceWriter);

		// Reduce into single list of properties.
		TArray<FReplicatedPropertyInfo> ReplicatedProperties;
		for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
		{
			ReplicatedProperties.Append(Layout.ReplicatedProperties[Group]);
		}

		// Create rep layout.
		SourceWriter.Printf(R"""(
			UClass* Class = FindObject<UClass>(ANY_PACKAGE, TEXT("%s"));
			FRepLayout RepLayout;
			RepLayout.InitFromObjectClass(Class);
			checkf(RepLayout.Cmds.Num() == %d, TEXT("RepLayout here does not match the RepLayout used when generating interop code."));)""",
			*Class->GetName(),
			ReplicatedProperties.Num() + 1 /* Need to add one here, because RepLayout.Cmds will contain the null terminator at the end. */);

		// Populate HandleToPropertyMap.
		for (auto& RepProp : ReplicatedProperties)
		{
			auto Handle = RepProp.Entry.Handle;
			if (RepProp.Entry.Parent)
			{
				SourceWriter.Printf("HandleToPropertyMap.Add(%d, FRepHandleData{Class->FindPropertyByName(\"%s\"), nullptr, %s, %s, 0});",
					Handle,
					*RepProp.Entry.Parent->GetName(),
					*GetLifetimeConditionAsString(RepProp.Entry.Condition),
					*GetRepNotifyLifetimeConditionAsString(RepProp.Entry.RepNotifyCondition));
				SourceWriter.Printf("HandleToPropertyMap[%d].Property = Cast<UStructProperty>(HandleToPropertyMap[%d].Parent)->Struct->FindPropertyByName(\"%s\");",
					Handle,
					Handle,
					*RepProp.Entry.Property->GetName());
			}
			else
			{
				SourceWriter.Printf("HandleToPropertyMap.Add(%d, FRepHandleData{nullptr, Class->FindPropertyByName(\"%s\"), %s, %s, 0});",
					Handle,
					*RepProp.Entry.Property->GetName(),
					*GetLifetimeConditionAsString(RepProp.Entry.Condition),
					*GetRepNotifyLifetimeConditionAsString(RepProp.Entry.RepNotifyCondition));
			}
			SourceWriter.Printf("HandleToPropertyMap[%d].Offset = RepLayout.Cmds[%d].Offset;", Handle, RepProp.Entry.CmdIndex);
		}
	}
	SourceWriter.Print("return HandleToPropertyMap;");
}

void GenerateFunction_GetBoundClass(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter Init(SourceWriter, {"UClass*", "GetBoundClass() const"}, TypeBindingName(Class));
	SourceWriter.Printf("return %s::StaticClass();", *GetFullCPPName(Class));
}

void GenerateFunction_Init(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout)
{
	FFunctionWriter Init(SourceWriter, {"void", "Init(USpatialInterop* InInterop, USpatialPackageMapClient* InPackageMap)"}, TypeBindingName(Class));

	SourceWriter.Print("Super::Init(InInterop, InPackageMap);");
	SourceWriter.Print();
	for (auto Group : GetRPCTypes())
	{
		for (auto& RPC : Layout.RPCs[Group])
		{
			SourceWriter.Printf("RPCToSenderMap.Emplace(\"%s\", &%s::%s_SendCommand);", *RPC.Function->GetName(), *TypeBindingName(Class), *RPC.Function->GetName());
		}
	}
}

void GenerateFunction_BindToView(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout)
{
	FFunctionWriter BindToView(SourceWriter, {"void", "BindToView()"}, TypeBindingName(Class));

	SourceWriter.Print("TSharedPtr<worker::View> View = Interop->GetSpatialOS()->GetView().Pin();");
	SourceWriter.Print("ViewCallbacks.Init(View);");
	SourceWriter.Print();
	SourceWriter.Print("if (Interop->GetNetDriver()->GetNetMode() == NM_Client)");
	{
		FScopeWriter ClientScope(SourceWriter);

		for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
		{
			SourceWriter.Printf("ViewCallbacks.Add(View->OnComponentUpdate<improbable::unreal::%s>([this](",
				*SchemaReplicatedDataName(Group, Class));
			SourceWriter.Indent();
			SourceWriter.Printf("const worker::ComponentUpdateOp<improbable::unreal::%s>& Op)",
				*SchemaReplicatedDataName(Group, Class));
			SourceWriter.Outdent();
			SourceWriter.Print("{");
			SourceWriter.Indent();
			SourceWriter.Printf(R"""(
				USpatialActorChannel* ActorChannel = Interop->GetActorChannelByEntityId(Op.EntityId);
				if (ActorChannel)
				{
					ClientReceiveUpdate_%s(ActorChannel, Op.Update);
				}
				else
				{
					Op.Update.ApplyTo(Pending%sData.FindOrAdd(Op.EntityId));
				})""",
				*GetReplicatedPropertyGroupName(Group),
				*GetReplicatedPropertyGroupName(Group));
			SourceWriter.Outdent();
			SourceWriter.Print("}));");
		}
	}

	for (auto Group : GetRPCTypes())
	{
		// Ensure that this class contains RPCs of the type specified by group (eg, Server or Client) so that we don't generate code for missing components
		if (Layout.RPCs.Contains(Group) && Layout.RPCs[Group].Num() > 0)
		{
			SourceWriter.Print();
			SourceWriter.Printf("using %sRPCCommandTypes = improbable::unreal::%s::Commands;",
				*GetRPCTypeName(Group),
				*SchemaRPCComponentName(Group, Class));
			for (auto& RPC : Layout.RPCs[Group])
			{
				SourceWriter.Printf("ViewCallbacks.Add(View->OnCommandRequest<%sRPCCommandTypes::%s>(std::bind(&%s::%s_OnCommandRequest, this, std::placeholders::_1)));",
					*GetRPCTypeName(Group),
					*CPPCommandClassName(RPC.Function),
					*TypeBindingName(Class),
					*RPC.Function->GetName());
			}
			for (auto& RPC : Layout.RPCs[Group])
			{
				SourceWriter.Printf("ViewCallbacks.Add(View->OnCommandResponse<%sRPCCommandTypes::%s>(std::bind(&%s::%s_OnCommandResponse, this, std::placeholders::_1)));",
					*GetRPCTypeName(Group),
					*CPPCommandClassName(RPC.Function),
					*TypeBindingName(Class),
					*RPC.Function->GetName());
			}
		}
	}
}

void GenerateFunction_UnbindFromView(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter UnbindFromView(SourceWriter, {"void", "UnbindFromView()"}, TypeBindingName(Class));
	SourceWriter.Print("ViewCallbacks.Reset();");
}

void GenerateFunction_GetReplicatedGroupComponentId(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout)
{
	FFunctionWriter GetReplicatedGroupComponentId(
		SourceWriter,
		{"worker::ComponentId", "GetReplicatedGroupComponentId(EReplicatedPropertyGroup Group) const"},
		TypeBindingName(Class));

	SourceWriter.Print("switch (Group)");
	{
		FScopeWriter SwitchScope(SourceWriter);

		for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
		{
			SourceWriter.Outdent();
			SourceWriter.Printf("case GROUP_%s:", *GetReplicatedPropertyGroupName(Group));
			SourceWriter.Indent();
			SourceWriter.Printf("return improbable::unreal::%s::ComponentId;", *SchemaReplicatedDataName(Group, Class));
		}
		SourceWriter.Outdent().Print("default:").Indent();
		SourceWriter.Print("checkNoEntry();");
		SourceWriter.Print("return 0;");
	}
}

void GenerateFunction_CreateActorEntity(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout)
{
	FFunctionWriter CreateActorEntity(
		SourceWriter,
		{"worker::Entity", "CreateActorEntity(const FString& ClientWorkerId, const FVector& Position, const FString& Metadata, const FPropertyChangeState& InitialChanges, USpatialActorChannel* Channel) const"},
		TypeBindingName(Class));

	// Set up initial data.
	SourceWriter.Print(TEXT("// Setup initial data."));
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf("improbable::unreal::%s::Data %sData;",
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group));
		SourceWriter.Printf("improbable::unreal::%s::Update %sUpdate;",
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group));
		SourceWriter.Printf("bool b%sUpdateChanged = false;", *GetReplicatedPropertyGroupName(Group));
	}
	TArray<FString> BuildUpdateArgs;
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		BuildUpdateArgs.Add(FString::Printf(TEXT("%sUpdate"), *GetReplicatedPropertyGroupName(Group)));
		BuildUpdateArgs.Add(FString::Printf(TEXT("b%sUpdateChanged"), *GetReplicatedPropertyGroupName(Group)));
	}
	SourceWriter.Printf("BuildSpatialComponentUpdate(InitialChanges, Channel, %s);", *FString::Join(BuildUpdateArgs, TEXT(", ")));
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf("%sUpdate.ApplyTo(%sData);",
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group));
	}

	// Create Entity.
	SourceWriter.Print();
	SourceWriter.Print(R"""(
		// Create entity.
		std::string ClientWorkerIdString = TCHAR_TO_UTF8(*ClientWorkerId);

		improbable::WorkerAttributeSet WorkerAttribute{{worker::List<std::string>{"UnrealWorker"}}};
		improbable::WorkerAttributeSet ClientAttribute{{worker::List<std::string>{"UnrealClient"}}};
		improbable::WorkerAttributeSet OwningClientAttribute{{"workerId:" + ClientWorkerIdString}};

		improbable::WorkerRequirementSet WorkersOnly{{WorkerAttribute}};
		improbable::WorkerRequirementSet ClientsOnly{{ClientAttribute}};
		improbable::WorkerRequirementSet OwningClientOnly{{OwningClientAttribute}};
		improbable::WorkerRequirementSet AnyUnrealWorkerOrClient{{WorkerAttribute, ClientAttribute}};
		improbable::WorkerRequirementSet AnyUnrealWorkerOrOwningClient{{WorkerAttribute, OwningClientAttribute}};

		// Set up unreal metadata.
		improbable::unreal::UnrealMetadata::Data UnrealMetadata;
		if (Channel->Actor->IsFullNameStableForNetworking())
		{
			UnrealMetadata.set_static_path({std::string{TCHAR_TO_UTF8(*Channel->Actor->GetPathName(Channel->Actor->GetWorld()))}});
		}
		if (!ClientWorkerIdString.empty())
		{
			UnrealMetadata.set_owner_worker_id({ClientWorkerIdString});
		}

		uint32 CurrentOffset = 1;
		worker::Map<std::string, std::uint32_t> SubobjectNameToOffset;
		ForEachObjectWithOuter(Channel->Actor, [&UnrealMetadata, &CurrentOffset, &SubobjectNameToOffset](UObject* Object)
		{
			// Objects can only be allocated NetGUIDs if this is true.
			if (Object->IsSupportedForNetworking() && !Object->IsPendingKill() && !Object->IsEditorOnly())
			{
				SubobjectNameToOffset.emplace(TCHAR_TO_UTF8(*(Object->GetName())), CurrentOffset);
				CurrentOffset++;
			}
		});
		UnrealMetadata.set_subobject_name_to_offset(SubobjectNameToOffset);
		
		// Build entity.
		const improbable::Coordinates SpatialPosition = SpatialConstants::LocationToSpatialOSCoordinates(Position);)""");
	SourceWriter.Print("return improbable::unreal::FEntityBuilder::Begin()");
	SourceWriter.Indent();
	SourceWriter.Printf(R"""(
		.AddPositionComponent(improbable::Position::Data{SpatialPosition}, WorkersOnly)
		.AddMetadataComponent(improbable::Metadata::Data{TCHAR_TO_UTF8(*Metadata)})
		.SetPersistence(true)
		.SetReadAcl(%s)
		.AddComponent<improbable::unreal::UnrealMetadata>(UnrealMetadata, WorkersOnly))""",
		Class->GetName() == TEXT("PlayerController") ? TEXT("AnyUnrealWorkerOrOwningClient") : TEXT("AnyUnrealWorkerOrClient"));
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf(".AddComponent<improbable::unreal::%s>(%sData, WorkersOnly)",
			*SchemaReplicatedDataName(Group, Class), *GetReplicatedPropertyGroupName(Group));
	}
	SourceWriter.Printf(".AddComponent<improbable::unreal::%s>(improbable::unreal::%s::Data{}, WorkersOnly)",
		*SchemaWorkerReplicatedDataName(Class), *SchemaWorkerReplicatedDataName(Class));
	SourceWriter.Printf(".AddComponent<improbable::unreal::%s>(improbable::unreal::%s::Data{}, OwningClientOnly)",
		*SchemaRPCComponentName(ERPCType::RPC_Client, Class), *SchemaRPCComponentName(ERPCType::RPC_Client, Class));
	SourceWriter.Printf(".AddComponent<improbable::unreal::%s>(improbable::unreal::%s::Data{}, WorkersOnly)",
		*SchemaRPCComponentName(ERPCType::RPC_Server, Class), *SchemaRPCComponentName(ERPCType::RPC_Server, Class));

	//This adds a custom component called PossessPawn which is added the the Character and Vehicle. It allows
	//these two classes to call an RPC which is intended to let the player possess a different pawn.
	if (Class->GetName().Contains("WheeledVehicle") || Class->GetName().Contains("Character"))
	{
		SourceWriter.Printf(".AddComponent<nuf::PossessPawn>(nuf::PossessPawn::Data{}, WorkersOnly)");
	}

	SourceWriter.Print(".Build();");
	SourceWriter.Outdent();
}

void GenerateFunction_SendComponentUpdates(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter SendComponentUpdates(
		SourceWriter,
		{"void", "SendComponentUpdates(const FPropertyChangeState& Changes, USpatialActorChannel* Channel, const FEntityId& EntityId) const"},
		TypeBindingName(Class));

	SourceWriter.Print("// Build SpatialOS updates.");
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf("improbable::unreal::%s::Update %sUpdate;",
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group));
		SourceWriter.Printf("bool b%sUpdateChanged = false;", *GetReplicatedPropertyGroupName(Group));
	}

	TArray<FString> BuildUpdateArgs;
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		BuildUpdateArgs.Add(FString::Printf(TEXT("%sUpdate"), *GetReplicatedPropertyGroupName(Group)));
		BuildUpdateArgs.Add(FString::Printf(TEXT("b%sUpdateChanged"), *GetReplicatedPropertyGroupName(Group)));
	}
	SourceWriter.Printf("BuildSpatialComponentUpdate(Changes, Channel, %s);", *FString::Join(BuildUpdateArgs, TEXT(", ")));

	SourceWriter.Print();
	SourceWriter.Print("// Send SpatialOS updates if anything changed.");
	SourceWriter.Print("TSharedPtr<worker::Connection> Connection = Interop->GetSpatialOS()->GetConnection().Pin();");
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf(R"""(
			if (b%sUpdateChanged)
			{
				Connection->SendComponentUpdate<improbable::unreal::%s>(EntityId.ToSpatialEntityId(), %sUpdate);
			})""",
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group));
	}
}

void GenerateFunction_SendRPCCommand(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter SendRPCCommand(
		SourceWriter,
		{"void", "SendRPCCommand(UObject* TargetObject, const UFunction* const Function, FFrame* const Frame)"},
		TypeBindingName(Class));

	SourceWriter.Print(R"""(
		TSharedPtr<worker::Connection> Connection = Interop->GetSpatialOS()->GetConnection().Pin();
		auto SenderFuncIterator = RPCToSenderMap.Find(Function->GetFName());
		checkf(*SenderFuncIterator, TEXT("Sender for %s has not been registered with RPCToSenderMap."), *Function->GetFName().ToString());
		(this->*(*SenderFuncIterator))(Connection.Get(), Frame, TargetObject);)""");
}

void GenerateFunction_ReceiveAddComponent(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter ReceiveAddComponent(
		SourceWriter,
		{"void", "ReceiveAddComponent(USpatialActorChannel* Channel, UAddComponentOpWrapperBase* AddComponentOp) const"},
		TypeBindingName(Class));

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf(R"""(
				auto* %sAddOp = Cast<U%sAddComponentOp>(AddComponentOp);
				if (%sAddOp)
				{
					auto Update = improbable::unreal::%s::Update::FromInitialData(*%sAddOp->Data.data());
					ClientReceiveUpdate_%s(Channel, Update);
				})""",
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group));
	}
}

void GenerateFunction_ApplyQueuedStateToChannel(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionWriter ApplyQueuedStateToChannel(
		SourceWriter,
		{"void", "ApplyQueuedStateToChannel(USpatialActorChannel* ActorChannel)"},
		TypeBindingName(Class));

	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		SourceWriter.Printf(R"""(
			improbable::unreal::%s::Data* %sData = Pending%sData.Find(ActorChannel->GetEntityId());
			if (%sData)
			{
				auto Update = improbable::unreal::%s::Update::FromInitialData(*%sData);
				Pending%sData.Remove(ActorChannel->GetEntityId());
				ClientReceiveUpdate_%s(ActorChannel, Update);
			})""",
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group));
	}
}

void GenerateFunction_BuildSpatialComponentUpdate(FCodeWriter& SourceWriter, UClass* Class)
{
	FFunctionSignature BuildComponentUpdateSignature;
	BuildComponentUpdateSignature.Type = "void";
	BuildComponentUpdateSignature.NameAndParams = "BuildSpatialComponentUpdate(\n\tconst FPropertyChangeState& Changes,\n\tUSpatialActorChannel* Channel";
	for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
	{
		BuildComponentUpdateSignature.NameAndParams += FString::Printf(TEXT(",\n\timprobable::unreal::%s::Update& %sUpdate,\n\tbool& b%sUpdateChanged"),
			*SchemaReplicatedDataName(Group, Class),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group),
			*GetReplicatedPropertyGroupName(Group));
	}
	BuildComponentUpdateSignature.NameAndParams += ") const";

	FFunctionWriter BuildComponentFunction(SourceWriter, BuildComponentUpdateSignature, TypeBindingName(Class));

	SourceWriter.Print(R"""(
			// Build up SpatialOS component updates.
			auto& PropertyMap = GetHandlePropertyMap();
			FChangelistIterator ChangelistIterator(Changes.Changed, 0);
			FRepHandleIterator HandleIterator(ChangelistIterator, Changes.Cmds, Changes.BaseHandleToCmdIndex, 0, 1, 0, Changes.Cmds.Num() - 1);
			while (HandleIterator.NextHandle()))""");
	{
		FScopeWriter WhileLoop(SourceWriter);
		SourceWriter.Print(R"""(
				const FRepLayoutCmd& Cmd = Changes.Cmds[HandleIterator.CmdIndex];
				const uint8* Data = Changes.SourceData + HandleIterator.ArrayOffset + Cmd.Offset;
				auto& PropertyMapData = PropertyMap[HandleIterator.Handle];
				UE_LOG(LogSpatialOSInterop, Verbose, TEXT("%s: Sending property update. actor %s (%lld), property %s (handle %d)"),
					*Interop->GetSpatialOS()->GetWorkerId(),
					*Channel->Actor->GetName(),
					Channel->GetEntityId().ToSpatialEntityId(),
					*Cmd.Property->GetName(),
					HandleIterator.Handle);)""");

		SourceWriter.Print("switch (GetGroupFromCondition(PropertyMapData.Condition))");
		{
			FScopeWriter SwitchScope(SourceWriter);
			for (EReplicatedPropertyGroup Group : GetAllReplicatedPropertyGroups())
			{
				SourceWriter.Outdent();
				SourceWriter.Printf("case GROUP_%s:", *GetReplicatedPropertyGroupName(Group));
				SourceWriter.Indent();
				SourceWriter.Printf("ServerSendUpdate_%s(Data, HandleIterator.Handle, Cmd.Property, Channel, %sUpdate);",
					*GetReplicatedPropertyGroupName(Group),
					*GetReplicatedPropertyGroupName(Group));
				SourceWriter.Printf("b%sUpdateChanged = true;",
					*GetReplicatedPropertyGroupName(Group));
				SourceWriter.Print("break;");
			}
		}
	}
}

void GenerateFunction_ServerSendUpdate(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout, EReplicatedPropertyGroup Group)
{
	FFunctionSignature ServerSendUpdateSignature
	{
		"void",
		FString::Printf(TEXT("ServerSendUpdate_%s(const uint8* RESTRICT Data, int32 Handle, UProperty* Property, USpatialActorChannel* Channel, improbable::unreal::%s::Update& OutUpdate) const;"),
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class))
	};
	FFunctionWriter ServerSendUpdate(SourceWriter, ServerSendUpdateSignature, TypeBindingName(Class));

	if (Layout.ReplicatedProperties[Group].Num() > 0)
	{
		SourceWriter.Print("switch (Handle)");
		{
			FScopeWriter SwitchScope(SourceWriter);
			for (auto& RepProp : Layout.ReplicatedProperties[Group])
			{
				auto Handle = RepProp.Entry.Handle;
				UProperty* Property = RepProp.Entry.Property;

				SourceWriter.Printf("case %d: // %s", Handle, *SchemaFieldName(RepProp.Entry.Chain));
				SourceWriter.BeginScope();

				// Get unreal data by deserialising from the reader, convert and set the corresponding field in the update object.
				FString PropertyValueName = TEXT("Value");
				FString PropertyCppType = Property->GetClass()->GetFName().ToString();
				FString PropertyValueCppType = Property->GetCPPType();
				FString PropertyName = TEXT("Property");
				//todo-giray: The reinterpret_cast below is ugly and we believe we can do this more gracefully using Property helper functions.
				if (Property->IsA<UBoolProperty>())
				{
					SourceWriter.Printf("bool %s = static_cast<UBoolProperty*>(Property)->GetPropertyValue(Data);", *PropertyValueName);
				}
				else
				{
					SourceWriter.Printf("%s %s = *(reinterpret_cast<%s const*>(Data));", *PropertyValueCppType, *PropertyValueName, *PropertyValueCppType);
				}
				SourceWriter.Print();
				GenerateUnrealToSchemaConversion(
					SourceWriter, "OutUpdate", RepProp.Entry.Chain, PropertyValueName, true,
					[&SourceWriter, Handle](const FString& PropertyValue)
				{
					SourceWriter.Printf("Interop->QueueOutgoingObjectUpdate_Internal(%s, Channel, %d);", *PropertyValue, Handle);
				});
				SourceWriter.Print("break;");
				SourceWriter.End();
			}
			SourceWriter.Outdent().Print("default:");
			SourceWriter.Indent();
			SourceWriter.Print("checkf(false, TEXT(\"Unknown replication handle %d encountered when creating a SpatialOS update.\"));");
			SourceWriter.Print("break;");
		}
	}
}

void GenerateFunction_ReceiveUpdate(FCodeWriter& SourceWriter, UClass* Class, const FPropertyLayout& Layout, EReplicatedPropertyGroup Group)
{
	FFunctionSignature ReceiveUpdateSignature{"void",
		FString::Printf(TEXT("ReceiveUpdate_%s(USpatialActorChannel* ActorChannel, const improbable::unreal::%s::Update& Update) const;"),
			*GetReplicatedPropertyGroupName(Group),
			*SchemaReplicatedDataName(Group, Class))
	};

	FFunctionWriter ReceiveUpdate(SourceWriter, ReceiveUpdateSignature, TypeBindingName(Class));

	SourceWriter.Printf(R"""(
			Interop->PreReceiveSpatialUpdate(ActorChannel);

			TArray<UProperty*> RepNotifies;)""");
	if (Layout.ReplicatedProperties[Group].Num() > 0)
	{
		SourceWriter.Printf(R"""(
				const bool bIsServer = Interop->GetNetDriver()->IsServer();
				const bool bAutonomousProxy = ActorChannel->IsClientAutonomousProxy(improbable::unreal::%s::ComponentId);
				const FRepHandlePropertyMap& HandleToPropertyMap = GetHandlePropertyMap();
				FSpatialConditionMapFilter ConditionMap(ActorChannel, bAutonomousProxy);)""",
			*SchemaRPCComponentName(ERPCType::RPC_Client, Class));
		SourceWriter.Print();
		for (auto& RepProp : Layout.ReplicatedProperties[Group])
		{
			auto Handle = RepProp.Entry.Handle;
			UProperty* Property = RepProp.Entry.Property;

			// Check if only the first property is in the property list. This implies that the rest is also in the update, as
			// they are sent together atomically.
			SourceWriter.Printf("if (!Update.%s().empty())", *SchemaFieldName(RepProp.PropertyList[0].Chain));
			{
				FScopeWriter UpdateNotEmptyScope(SourceWriter);

				// Check if the property is relevant on the client.
				SourceWriter.Printf("// %s", *SchemaFieldName(RepProp.Entry.Chain));
				SourceWriter.Printf("uint16 Handle = %d;", Handle);
				SourceWriter.Print("const FRepHandleData* RepData = &HandleToPropertyMap[Handle];");
				SourceWriter.Print("if (bIsServer || ConditionMap.IsRelevant(RepData->Condition))");
				{
					FScopeWriter PropertyIsRelevantScope(SourceWriter);

					if (Property->IsA<UObjectPropertyBase>())
					{
						SourceWriter.Print("bool bWriteObjectProperty = true;");
					}

					// If the property is Role or RemoteRole, ensure to swap on the client.
					int SwappedHandleIndex = -1;
					if (Property->GetFName() == NAME_RemoteRole)
					{
						// Find handle to role.
						for (auto& OtherRepProp : Layout.ReplicatedProperties[Group])
						{
							if (OtherRepProp.Entry.Property->GetFName() == NAME_Role)
							{
								SwappedHandleIndex = OtherRepProp.Entry.Handle;
								break;
							}
						}
					}
					if (Property->GetFName() == NAME_Role)
					{
						// Find handle to remote role.
						for (auto& OtherRepProp : Layout.ReplicatedProperties[Group])
						{
							if (OtherRepProp.Entry.Property->GetFName() == NAME_RemoteRole)
							{
								SwappedHandleIndex = OtherRepProp.Entry.Handle;
								break;
							}
						}
					}
					if (SwappedHandleIndex != -1)
					{
						SourceWriter.Printf(R"""(
							// On the client, we need to swap Role/RemoteRole.
							if (!bIsServer)
							{
								Handle = %d;
								RepData = &HandleToPropertyMap[Handle];
							})""", SwappedHandleIndex);
						SourceWriter.Print();
					}

					// Convert update data to the corresponding Unreal type and serialize to OutputWriter.
					FString PropertyValueName = TEXT("Value");
					FString PropertyValueCppType = Property->GetCPPType();
					FString PropertyName = TEXT("RepData->Property");
					//todo-giray: The reinterpret_cast below is ugly and we believe we can do this more gracefully using Property helper functions.
					SourceWriter.Printf("uint8* PropertyData = reinterpret_cast<uint8*>(ActorChannel->Actor) + RepData->Offset;");
					if (Property->IsA<UBoolProperty>())
					{
						SourceWriter.Printf("bool %s = static_cast<UBoolProperty*>(%s)->GetPropertyValue(PropertyData);", *PropertyValueName, *PropertyName);
					}
					else
					{
						SourceWriter.Printf("%s %s = *(reinterpret_cast<%s const*>(PropertyData));", *PropertyValueCppType, *PropertyValueName, *PropertyValueCppType);
					}
					SourceWriter.Print();
					GeneratePropertyToUnrealConversion(
						SourceWriter, TEXT("Update"), RepProp.Entry.Chain, PropertyValueName, true, PropertyValueCppType,
						[&SourceWriter](const FString& PropertyValue)
					{
						SourceWriter.Print(R"""(
							UE_LOG(LogSpatialOSInterop, Log, TEXT("%s: Received unresolved object property. Value: %s. actor %s (%lld), property %s (handle %d)"),
								*Interop->GetSpatialOS()->GetWorkerId(),
								*ObjectRefToString(ObjectRef),
								*ActorChannel->Actor->GetName(),
								ActorChannel->GetEntityId().ToSpatialEntityId(),
								*RepData->Property->GetName(),
								Handle);)""");
						SourceWriter.Print("bWriteObjectProperty = false;");
						SourceWriter.Print("Interop->QueueIncomingObjectUpdate_Internal(ObjectRef, ActorChannel, RepData);");
					});

					// If this is RemoteRole, make sure to downgrade if bAutonomousProxy is false.
					if (Property->GetFName() == NAME_RemoteRole)
					{
						SourceWriter.Print();
						SourceWriter.Print(R"""(
							// Downgrade role from AutonomousProxy to SimulatedProxy if we aren't authoritative over
							// the server RPCs component.
							if (!bIsServer && Value == ROLE_AutonomousProxy && !bAutonomousProxy)
							{
								Value = ROLE_SimulatedProxy;
							})""");
					}

					SourceWriter.Print();

					if (Property->IsA<UObjectPropertyBase>())
					{
						SourceWriter.Print("if (bWriteObjectProperty)");
						SourceWriter.BeginScope();
					}

					SourceWriter.Print("ApplyIncomingPropertyUpdate(*RepData, ActorChannel->Actor, static_cast<const void*>(&Value), RepNotifies);");
					SourceWriter.Print();

					SourceWriter.Print(R"""(
						UE_LOG(LogSpatialOSInterop, Verbose, TEXT("%s: Received property update. actor %s (%lld), property %s (handle %d)"),
							*Interop->GetSpatialOS()->GetWorkerId(),
							*ActorChannel->Actor->GetName(),
							ActorChannel->GetEntityId().ToSpatialEntityId(),
							*RepData->Property->GetName(),
							Handle);)""");

					if (Property->IsA<UObjectPropertyBase>())
					{
						SourceWriter.End();
					}
				}
			}
		}
	}
	SourceWriter.Print("Interop->PostReceiveSpatialUpdate(ActorChannel, RepNotifies);");
}

void GenerateFunction_RPCSendCommand(FCodeWriter& SourceWriter, UClass* Class, const FRPCDefinition& RPC)
{
	FFunctionSignature SendCommandSignature{
		"void",
		FString::Printf(TEXT("%s_SendCommand(worker::Connection* const Connection, struct FFrame* const RPCFrame, UObject* TargetObject)"),
			*RPC.Function->GetName())
	};
	FFunctionWriter SendCommand(SourceWriter, SendCommandSignature, TypeBindingName(Class));

	// Extract RPC arguments from the stack.
	if (RPC.Function->NumParms > 0)
	{
		// Note that macros returned by GeneratePropertyReader require this FFrame variable to be named "Stack"
		SourceWriter.Print("FFrame& Stack = *RPCFrame;");
		for (TFieldIterator<UProperty> Param(RPC.Function); Param; ++Param)
		{
			SourceWriter.Print(*GeneratePropertyReader(*Param));
		}
		SourceWriter.Print();
	}

	// Build closure to send the command request.
	TArray<FString> CapturedArguments;
	CapturedArguments.Add(TEXT("TargetObject"));
	for (TFieldIterator<UProperty> Param(RPC.Function); Param; ++Param)
	{
		CapturedArguments.Add((*Param)->GetName());
	}
	SourceWriter.Printf("auto Sender = [this, Connection, %s]() mutable -> FRPCCommandRequestResult", *FString::Join(CapturedArguments, TEXT(", ")));
	SourceWriter.Print("{").Indent();

	SourceWriter.Printf(R"""(
		// Resolve TargetObject.
		improbable::unreal::UnrealObjectRef TargetObjectRef = PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject));
		if (TargetObjectRef == SpatialConstants::UNRESOLVED_OBJECT_REF)
		{
			UE_LOG(LogSpatialOSInterop, Log, TEXT("%%s: RPC %s queued. Target object is unresolved."), *Interop->GetSpatialOS()->GetWorkerId());
			return {TargetObject};
		})""", *RPC.Function->GetName());
	SourceWriter.Print();
	SourceWriter.Print("// Build request.");
	SourceWriter.Printf("improbable::unreal::%s Request;", *SchemaRPCRequestType(RPC.Function));
	for (TFieldIterator<UProperty> Param(RPC.Function); Param; ++Param)
	{
		TArray<UProperty*> NewChain = {*Param};
		GenerateUnrealToSchemaConversion(
			SourceWriter, "Request", NewChain, *Param->GetNameCPP(), false,
			[&SourceWriter, &RPC](const FString& PropertyValue)
		{
			SourceWriter.Printf("UE_LOG(LogSpatialOSInterop, Log, TEXT(\"%%s: RPC %s queued. %s is unresolved.\"), *Interop->GetSpatialOS()->GetWorkerId());",
				*RPC.Function->GetName(),
				*PropertyValue);
			SourceWriter.Printf("return {%s};", *PropertyValue);
		});
	}
	SourceWriter.Print();
	SourceWriter.Printf(R"""(
		// Send command request.
		Request.set_target_subobject_offset(TargetObjectRef.offset());
		UE_LOG(LogSpatialOSInterop, Verbose, TEXT("%%s: Sending RPC: %s, target: %%s %%s"),
			*Interop->GetSpatialOS()->GetWorkerId(),
			*TargetObject->GetName(),
			*ObjectRefToString(TargetObjectRef));
		auto RequestId = Connection->SendCommandRequest<improbable::unreal::%s::Commands::%s>(TargetObjectRef.entity(), Request, 0);
		return {RequestId.Id};)""",
		*RPC.Function->GetName(),
		*SchemaRPCComponentName(RPC.Type, Class),
		*CPPCommandClassName(RPC.Function));
	SourceWriter.Outdent().Print("};");
	SourceWriter.Printf("Interop->SendCommandRequest_Internal(Sender, %s);", RPC.bReliable ? TEXT("/*bReliable*/ true") : TEXT("/*bReliable*/ false"));
}

void GenerateFunction_RPCOnCommandRequest(FCodeWriter& SourceWriter, UClass* Class, const FRPCDefinition& RPC)
{
	FString RequestFuncName = FString::Printf(TEXT("%s_OnCommandRequest(const worker::CommandRequestOp<improbable::unreal::%s::Commands::%s>& Op)"),
		*RPC.Function->GetName(),
		*SchemaRPCComponentName(RPC.Type, Class),
		*CPPCommandClassName(RPC.Function));
	FFunctionWriter Request(SourceWriter, {"void", RequestFuncName}, TypeBindingName(Class));

	// Generate receiver function.
	SourceWriter.Print("auto Receiver = [this, Op]() mutable -> FRPCCommandResponseResult");
	SourceWriter.Print("{").Indent();

	auto ObjectResolveFailureGenerator = [&SourceWriter, &RPC, Class](const FString& PropertyName, const FString& ObjectRef)
	{
		SourceWriter.Printf(R"""(
			UE_LOG(LogSpatialOSInterop, Log, TEXT("%%s: %s_OnCommandRequest: %s %%s is not resolved on this worker."),
				*Interop->GetSpatialOS()->GetWorkerId(),
				*ObjectRefToString(%s));
			return {%s};)""",
			*RPC.Function->GetName(),
			*PropertyName,
			*ObjectRef,
			*ObjectRef,
			*ObjectRef);
	};

	// Get the target object.
	SourceWriter.Printf(R"""(
		improbable::unreal::UnrealObjectRef TargetObjectRef{Op.EntityId, Op.Request.target_subobject_offset()};
		FNetworkGUID TargetNetGUID = PackageMap->GetNetGUIDFromUnrealObjectRef(TargetObjectRef);
		if (!TargetNetGUID.IsValid()))""");
	SourceWriter.Print("{").Indent();
	ObjectResolveFailureGenerator("Target object", "TargetObjectRef");
	SourceWriter.Outdent().Print("}");
	SourceWriter.Printf(R"""(
		UObject* TargetObjectUntyped = PackageMap->GetObjectFromNetGUID(TargetNetGUID, false);
		%s* TargetObject = Cast<%s>(TargetObjectUntyped);
		checkf(TargetObjectUntyped, TEXT("%%s: %s_OnCommandRequest: Object Ref %%s (NetGUID %%s) does not correspond to a UObject."),
			*Interop->GetSpatialOS()->GetWorkerId(),
			*ObjectRefToString(TargetObjectRef),
			*TargetNetGUID.ToString());
		checkf(TargetObject, TEXT("%%s: %s_OnCommandRequest: Object Ref %%s (NetGUID %%s) is the wrong type. Name: %%s"),
			*Interop->GetSpatialOS()->GetWorkerId(),
			*ObjectRefToString(TargetObjectRef),
			*TargetNetGUID.ToString(),
			*TargetObjectUntyped->GetName());)""",
		*GetFullCPPName(RPC.CallerType),
		*GetFullCPPName(RPC.CallerType),
		*RPC.Function->GetName(),
		*RPC.Function->GetName());

	// Grab RPC arguments.
	TArray<FString> RPCParameters;
	for (TFieldIterator<UProperty> Param(RPC.Function); Param; ++Param)
	{
		FString PropertyValueName = Param->GetNameCPP();
		FString PropertyValueCppType = Param->GetCPPType();
		FString PropertyName = TEXT("Data.Property");

		// Extract parameter.
		SourceWriter.Print();
		SourceWriter.Printf("// Extract %s", *Param->GetName());
		SourceWriter.Printf("%s %s;", *PropertyValueCppType, *PropertyValueName);
		GeneratePropertyToUnrealConversion(
			SourceWriter, "Op.Request", {*Param}, PropertyValueName, false, PropertyValueCppType,
			std::bind(ObjectResolveFailureGenerator, std::placeholders::_1, "ObjectRef"));

		// Append to parameter list.
		RPCParameters.Add(Param->GetNameCPP());
	}
	SourceWriter.Print();
	SourceWriter.Print("// Call implementation.");
	SourceWriter.Printf(R"""(
				UE_LOG(LogSpatialOSInterop, Verbose, TEXT("%%s: Received RPC: %s, target: %%s %%s"),
					*Interop->GetSpatialOS()->GetWorkerId(),
					*TargetObject->GetName(),
					*ObjectRefToString(TargetObjectRef));)""",
		*RPC.Function->GetName());
	SourceWriter.Printf("TargetObject->%s_Implementation(%s);",
		*RPC.Function->GetName(), *FString::Join(RPCParameters, TEXT(", ")));
	SourceWriter.Print();
	SourceWriter.Print("// Send command response.");
	SourceWriter.Print("TSharedPtr<worker::Connection> Connection = Interop->GetSpatialOS()->GetConnection().Pin();");
	SourceWriter.Printf("Connection->SendCommandResponse<improbable::unreal::%s::Commands::%s>(Op.RequestId, {});",
		*SchemaRPCComponentName(RPC.Type, Class),
		*CPPCommandClassName(RPC.Function));
	SourceWriter.Print("return {};");
	SourceWriter.Outdent().Print("};");

	SourceWriter.Print("Interop->SendCommandResponse_Internal(Receiver);");
}

void GenerateFunction_RPCOnCommandResponse(FCodeWriter& SourceWriter, UClass* Class, const FRPCDefinition& RPC)
{
	FString ResponseFuncName = FString::Printf(TEXT("%s_OnCommandResponse(const worker::CommandResponseOp<improbable::unreal::%s::Commands::%s>& Op)"),
		*RPC.Function->GetName(),
		*SchemaRPCComponentName(RPC.Type, Class),
		*CPPCommandClassName(RPC.Function));
	FFunctionWriter Response(SourceWriter, {"void", ResponseFuncName}, TypeBindingName(Class));

	SourceWriter.Printf("Interop->HandleCommandResponse_Internal(TEXT(\"%s\"), Op.RequestId.Id, Op.EntityId, Op.StatusCode, FString(UTF8_TO_TCHAR(Op.Message.c_str())));",
		*RPC.Function->GetName());
}
