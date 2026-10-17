/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Rig/RigVMGraph.h"

#if REFLECTION_CONTROL_RIG

#include "Engine/ControlRigCompatibility.h"

#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"

#include "Engine/Log.h"
#include "Internationalization/Regex.h"
#include "Serializers/PropertySerializer.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UObjectIterator.h"

namespace {
	/* The registry grew a read and a write half in 5.5, but the half it hands out keeps the
	 * locking accessors and deletes the unlocked ones, so the plain registry is what every
	 * version is asked for. */
	FORCEINLINE decltype(auto) GetRigVMRegistry() {
		return FRigVMRegistry::Get();
	}

	/* What a dispatch's arguments are called, and which way each one points.
	 *
	 * 5.4 moved both onto an info struct the factory hands back, and before that the arguments
	 * themselves carry them. 5.7 made the call take the registry handle the caller is reading
	 * under rather than reach for the registry itself. Only the name and the direction are read
	 * here, so one shape is built out of whichever the engine offers. */
	struct FDispatchArgument {
		FName Name;
		ERigVMPinDirection Direction;
	};

	TArray<FDispatchArgument> GetFactoryArgumentInfos(const FRigVMDispatchFactory* Factory) {
		TArray<FDispatchArgument> Built;

#if UE5_7_BEYOND
		FRigVMRegistryReadLock ReadLock(FRigVMRegistry::Get());

		for (const FRigVMTemplateArgumentInfo& Info : Factory->GetArgumentInfos(ReadLock)) {
			Built.Add({ Info.Name, Info.Direction });
		}
#elif UE5_4_BEYOND
		for (const FRigVMTemplateArgumentInfo& Info : Factory->GetArgumentInfos()) {
			Built.Add({ Info.Name, Info.Direction });
		}
#else
		for (const FRigVMTemplateArgument& Argument : Factory->GetArguments()) {
			Built.Add({ Argument.GetName(), Argument.GetDirection() });
		}
#endif

		return Built;
	}

	/* 5.4 grew a say over whether a pin linked to this one takes the value too */
	bool RigSetPinDefault(URigVMController* Controller, const FString& PinPath, const FString& Value) {
#if UE5_4_BEYOND
		return Controller->SetPinDefaultValue(PinPath, Value, true, false, false, false, false);
#else
		return Controller->SetPinDefaultValue(PinPath, Value, true, false, false, false);
#endif
	}

	FName GetFunctionArgumentName(const FRigVMFunction* Function, const int32 OperandIndex, const int32 OperandCount) {
#if UE5_7_BEYOND
		FRigVMRegistryReadLock ReadLock(FRigVMRegistry::Get());
		return Function->GetArgumentNameForOperandIndex(OperandIndex, OperandCount, ReadLock);
#else
		return Function->GetArgumentNameForOperandIndex(OperandIndex, OperandCount);
#endif
	}

	/* A type as the bytecode spells it (TArray<FName>, FUserDefinedStruct_2C641F3D47D778912879BE98B68E1D07): the
	 * registry's, or a user-defined struct's by the GUID RigVM names it after, which the registry only knows once
	 * something registered it. False for a type neither knows. */
	bool ResolveTypeString(const FString& TypeString, FString& OutCPPType, UObject*& OutObject) {
		const auto& Registry = GetRigVMRegistry();
		const TRigVMTypeIndex TypeIndex = Registry.GetTypeIndexFromCPPType(TypeString);

		if (TypeIndex != INDEX_NONE) {
			const FRigVMTemplateArgumentType& Type = Registry.GetType(TypeIndex);
			OutCPPType = Type.CPPType.ToString();
			OutObject = Type.CPPTypeObject;
			return true;
		}

		static const FRegexPattern UserStruct(TEXT("FUserDefinedStruct_([0-9A-F]{32})"));
		FRegexMatcher Matcher(UserStruct, TypeString);

		if (!Matcher.FindNext()) return false;

		const FString Guid = Matcher.GetCaptureGroup(1);

		for (TObjectIterator<UUserDefinedStruct> It; It; ++It) {
			if (It->GetOutermost() == GetTransientPackage() || It->Guid.ToString(EGuidFormats::Digits) != Guid) continue;

			const FString Base = RigVMTypeUtils::GetUniqueStructTypeName(*It);
			OutCPPType = TypeString.StartsWith(TEXT("TArray<")) ? RigVMTypeUtils::ArrayTypeFromBaseType(Base) : Base;
			OutObject = *It;
			return true;
		}

		return false;
	}

	/* Opcodes are one enum with every deprecated per-operand execute in front of them, so the ones
	 * that are still written carry numbers well past where they read */
	constexpr int32 OpCodeCopy = 68;
	constexpr int32 OpCodeExecute = 101;
	constexpr int32 OpCodeRunInstructions = 102;
	constexpr int32 OpCodeInvokeCallable = 104;

	/* What a register operand says when it names no sub property */
	constexpr int32 NoRegisterOffset = 65535;

	constexpr int32 MemoryTypeWork = 0;
	constexpr int32 MemoryTypeLiteral = 1;
	constexpr int32 MemoryTypeExternal = 2;

	/* An entry is written out as the struct it is: (Name="Forwards Solve",InstructionIndex=243) */
	int32 ReadEntryInstruction(const FString& Entry) {
		FString Value;

		if (!Entry.Split(TEXT("InstructionIndex="), nullptr, &Value)) {
			return 0;
		}

		Value.Split(TEXT(")"), &Value, nullptr);

		return FCString::Atoi(*Value);
	}

	/* Numbers come back out of json as doubles, and a pin wants the text the type reads back */
	FString ToDefaultValue(const TSharedPtr<FJsonValue>& Value, const URigVMPin* Pin) {
		if (!Value.IsValid()) {
			return FString();
		}

		switch (Value->Type) {
			case EJson::Boolean: {
				return Value->AsBool() ? TEXT("True") : TEXT("False");
			}

			case EJson::Number: {
				const double Number = Value->AsNumber();

				/* An integer pin refuses a decimal point, and a float one is written the way the
				 * editor writes it */
				if (Pin != nullptr && (Pin->GetCPPType() == TEXT("int32") || Pin->GetCPPType() == TEXT("uint8"))) {
					return FString::FromInt(static_cast<int32>(Number));
				}

				return FString::SanitizeFloat(Number);
			}

			case EJson::String: {
				FString String = Value->AsString();

				/* Enums are written as the type and the value, and a pin only wants the value */
				if (String.Contains(TEXT("::"))) {
					FString Right;

					if (String.Split(TEXT("::"), nullptr, &Right) && !Right.IsEmpty()) {
						String = Right;
					}
				}

				/* An exporter writes whichever name the value had first, aliases included:
				 * ERigElementType::First is Bone by another name, and the pin only reads the
				 * canonical one. Resolved through the enum so any alias lands on the real entry. */
				if (Pin != nullptr) {
					if (const UEnum* Enum = Cast<UEnum>(Pin->GetCPPTypeObject())) {
						const int64 EnumValue = Enum->GetValueByNameString(String);

						if (EnumValue != INDEX_NONE) {
							return Enum->GetNameStringByValue(EnumValue);
						}
					}
				}

				return String;
			}

			default: {
				return FString();
			}
		}
	}
}

FRigVMGraphReconstruction::FRigVMGraphReconstruction(UControlRigBlueprint* InBlueprint, UPropertySerializer* InPropertySerializer)
	: Blueprint(InBlueprint)
	, PropertySerializer(InPropertySerializer)
{
}

bool FRigVMGraphReconstruction::Build(const FUObjectJsonValueExport& VirtualMachine) {
	if (Blueprint == nullptr || !VirtualMachine.JsonObject.IsValid()) {
		return false;
	}

	Graph = Blueprint->GetDefaultModel();
	Controller = Blueprint->GetOrCreateController(Graph);

	if (Graph == nullptr || Controller == nullptr) {
		return false;
	}

	const FUObjectJsonValueExport ByteCode = VirtualMachine.GetObject(TEXT("ByteCodeStorage"));

	if (!ByteCode.JsonObject.IsValid() || !ByteCode.Has(TEXT("Instructions"))) {
		return false;
	}

	const TArray<FUObjectJsonValueExport> Instructions = ByteCode.GetArray(TEXT("Instructions"));

	TArray<FString> FunctionNames;

	if (VirtualMachine.Has(TEXT("FunctionNamesStorage"))) {
		for (const TSharedPtr<FJsonValue>& FunctionName : VirtualMachine.JsonObject->GetArrayField(TEXT("FunctionNamesStorage"))) {
			FunctionNames.Add(FunctionName->AsString());
		}
	}

	Stats.Instructions = Instructions.Num();

	ReadMemory(VirtualMachine);
	ReadCallables(ByteCode);

	/* The graph a new rig comes with holds the event it was created around, and a rig reflected
	 * over an earlier run holds that run's nodes. Both are cleared before this one puts the rig's
	 * own back.
	 *
	 * Handed to RemoveNodes rather than removed one at a time: the graph's node array is the one
	 * being emptied, so walking it while removing from it steps over every second node and leaves
	 * half of the previous run's graph behind, unlinked, on top of the new one. */
	Controller->RemoveNodes(Graph->GetNodes(), false, false);

	/* Auto recompilation is what would make this slow: a rig of any size is a few hundred nodes,
	 * and every one of them would otherwise recompile the VM on its way in */
	Blueprint->SetAutoVMRecompile(false);

	/* The functions go in first: their bodies are nodes like any other, only in their own graphs */
	CreateFunctions();
	CollectNodes(Instructions, FunctionNames);
	CreateNodes();
	ExposeArguments();
	CreateFunctionReferences();
	ApplyDefaults();

	/* Execution before values: every link refreshes the graph's diagnostics, which follow execution
	 * from the graph's event or a function's entry, and a mutable node reached through a value
	 * before its execution is there is reported as not linked to execution */
	CreateVariableSetters(Instructions);
	CreateExecution(Instructions, ByteCode);
	CreateLinks(Instructions);

	/* A getter is made for one reader, and one whose reader refused the link, a pin this engine
	 * build doesn't declare being how that happens, is left holding nothing */
	for (const TPair<FString, URigVMNode*>& Getter : VariableGetters) {
		if (Getter.Value == nullptr) continue;

		int32 GetterLinks = 0;

		for (const URigVMPin* Pin : Getter.Value->GetPins()) {
			GetterLinks += Pin->GetTargetLinks(true).Num() + Pin->GetSourceLinks(true).Num();
		}

		if (GetterLinks == 0) {
			GetController(Getter.Value)->RemoveNode(Getter.Value, false, false);

			Stats.VariableNodes--;
		}
	}

	ConsolidateDefaults();
	UpdateLayout(Graph);

	for (const FCallable& Callable : Callables) {
		if (Callable.Function != nullptr) {
			UpdateLayout(Callable.Function->GetContainedGraph());
		}
	}

	PromoteLocalVariables();

	Blueprint->SetAutoVMRecompile(true);

	return Stats.Nodes > 0;
}

void FRigVMGraphReconstruction::ReadMemory(const FUObjectJsonValueExport& VirtualMachine) {
	auto ReadStorage = [](const FUObjectJsonValueExport& Storage, TArray<FRegister>& OutRegisters, TArray<FString>& OutPropertyPaths) {
		if (!Storage.JsonObject.IsValid()) {
			return;
		}

		if (Storage.Has(TEXT("PropertyPathDescriptions"))) {
			for (const FUObjectJsonValueExport& PropertyPath : Storage.GetArray(TEXT("PropertyPathDescriptions"))) {
				OutPropertyPaths.Add(PropertyPath.Has(TEXT("SegmentPath")) ? PropertyPath.GetString(TEXT("SegmentPath")) : FString());
			}
		}

		if (!Storage.Has(TEXT("Properties"))) {
			return;
		}

		for (const FUObjectJsonValueExport& Property : Storage.GetArray(TEXT("Properties"))) {
			FRegister Register;

			if (Property.Has(TEXT("Name"))) {
				Register.Name = Property.GetString(TEXT("Name"));
			}

			if (Property.Has(TEXT("Tag"))) {
				Register.Value = Property.JsonObject->TryGetField(TEXT("Tag"));
			}

			const TSharedPtr<FJsonObject>* TagData = nullptr;

			if (Property.JsonObject->TryGetObjectField(TEXT("TagData"), TagData)) {
				Register.TagData = *TagData;
			}

			OutRegisters.Add(Register);
		}
	};

	ReadStorage(VirtualMachine.GetObject(TEXT("DefaultWorkMemoryStorage")), Work, WorkPropertyPaths);
	ReadStorage(VirtualMachine.GetObject(TEXT("LiteralMemoryStorage")), Literals, LiteralPropertyPaths);
}

void FRigVMGraphReconstruction::ReadCallables(const FUObjectJsonValueExport& ByteCode) {
	if (!ByteCode.Has(TEXT("CallableInfos"))) {
		return;
	}

	/* Listed in the order an invoke's index names them */
	for (const FUObjectJsonValueExport& Info : ByteCode.GetArray(TEXT("CallableInfos"))) {
		FCallable Callable;
		Callable.Name = Info.Has(TEXT("Name")) ? Info.GetString(TEXT("Name")) : FString();
		Callable.First = Info.GetInteger(TEXT("FirstInstruction"), INDEX_NONE);
		Callable.Last = Info.GetInteger(TEXT("LastInstruction"), INDEX_NONE);

		/* A function out of another asset's library is named by the path to it, the function last:
		 * /Game/.../FN_DeformRig_ControlRig_FNC.FN_DeformRig_ControlRig_FNC_C:FN_DeformRig_FNC */
		FString FunctionName;

		if (Callable.Name.Split(TEXT(":"), nullptr, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !FunctionName.IsEmpty()) {
			Callable.Name = FunctionName;
		}

		if (Callable.Name.IsEmpty()) {
			Callable.Name = FString::Printf(TEXT("Callable_%d"), Callables.Num());
		}

		if (Info.Has(TEXT("Arguments"))) {
			for (const FUObjectJsonValueExport& ArgumentInfo : Info.GetArray(TEXT("Arguments"))) {
				FCallableArgument Argument;
				Argument.Name = ArgumentInfo.Has(TEXT("Name")) ? ArgumentInfo.GetString(TEXT("Name")) : FString();
				Argument.TypeString = ArgumentInfo.Has(TEXT("TypeString")) ? ArgumentInfo.GetString(TEXT("TypeString")) : FString();

				/* ERigVMPinDirection: Input, Output, IO, in that order, written as its number or
				 * spelled out */
				const TSharedPtr<FJsonValue> DirectionValue = ArgumentInfo.JsonObject->TryGetField(TEXT("Direction"));
				int32 Direction = 0;

				if (DirectionValue.IsValid() && DirectionValue->Type == EJson::Number) {
					Direction = static_cast<int32>(DirectionValue->AsNumber());
				} else if (DirectionValue.IsValid() && DirectionValue->Type == EJson::String) {
					const FString Spelled = DirectionValue->AsString();

					Direction = Spelled.EndsWith(TEXT("Output")) ? 1 : Spelled.EndsWith(TEXT("IO")) ? 2 : 0;
				}

				Argument.bInput = Direction == 0 || Direction == 2;
				Argument.bOutput = Direction == 1 || Direction == 2;

				Argument.InterfaceRegister = ArgumentInfo.GetObject(TEXT("InterfaceOperand")).GetInteger(TEXT("RegisterIndex"), INDEX_NONE);
				Argument.ForwardedRegister = ArgumentInfo.GetObject(TEXT("ForwardedOperand")).GetInteger(TEXT("RegisterIndex"), INDEX_NONE);

				Callable.Arguments.Add(Argument);
			}
		}

		Callables.Add(Callable);
	}
}

int32 FRigVMGraphReconstruction::FindCallable(const int32 Instruction) const {
	int32 Innermost = INDEX_NONE;

	for (int32 Index = 0; Index < Callables.Num(); ++Index) {
		const FCallable& Callable = Callables[Index];

		if (Instruction < Callable.First || Instruction > Callable.Last) continue;

		if (Innermost == INDEX_NONE || (Callable.Last - Callable.First) < (Callables[Innermost].Last - Callables[Innermost].First)) {
			Innermost = Index;
		}
	}

	return Innermost;
}

URigVMGraph* FRigVMGraphReconstruction::GetCallableGraph(const int32 Callable) const {
	if (Callables.IsValidIndex(Callable) && Callables[Callable].Function != nullptr) {
		if (URigVMGraph* ContainedGraph = Callables[Callable].Function->GetContainedGraph()) {
			return ContainedGraph;
		}
	}

	return Graph;
}

URigVMController* FRigVMGraphReconstruction::GetController(const int32 Callable) const {
	return Blueprint->GetOrCreateController(GetCallableGraph(Callable));
}

URigVMController* FRigVMGraphReconstruction::GetController(const URigVMNode* Node) const {
	if (Node == nullptr || Node->GetGraph() == nullptr || Node->GetGraph() == Graph) {
		return Controller;
	}

	return Blueprint->GetOrCreateController(Node->GetGraph());
}

void FRigVMGraphReconstruction::CreateFunctions() {
	URigVMFunctionLibrary* Library = Blueprint->GetLocalFunctionLibrary();

	if (Library == nullptr) {
		return;
	}

	URigVMController* LibraryController = Blueprint->GetOrCreateController(Library);

	if (LibraryController == nullptr) {
		return;
	}

	/* A rig reflected over an earlier run holds that run's functions, the same way its graph holds
	 * that run's nodes. Nothing references them any more: the graph was emptied first. */
	for (const URigVMLibraryNode* Existing : Library->GetFunctions()) {
		LibraryController->RemoveFunctionFromLibrary(Existing->GetFName(), false, false);
	}

	for (int32 Index = 0; Index < Callables.Num(); ++Index) {
		FCallable& Callable = Callables[Index];

		/* An empty callable is stepped over by the VM, and its invoke along with it */
		if (Callable.First == INDEX_NONE || Callable.First > Callable.Last) continue;

		/* Every function is made mutable: the VM runs an invoke where it sits in the bytecode,
		 * which is where a function with execution pins runs in the chain */
		Callable.Function = LibraryController->AddFunctionToLibrary(
			FName(*Callable.Name),
			true,
			FVector2D(0.0f, 200.0f * static_cast<float>(Index)),
			false,
			false
		);

		if (Callable.Function != nullptr) {
			Stats.Functions++;
		}
	}
}

void FRigVMGraphReconstruction::ExposeArguments() {
	const auto& Registry = GetRigVMRegistry();

	for (int32 CallableIndex = 0; CallableIndex < Callables.Num(); ++CallableIndex) {
		FCallable& Callable = Callables[CallableIndex];

		if (Callable.Function == nullptr) continue;

		URigVMGraph* FunctionGraph = Callable.Function->GetContainedGraph();
		URigVMController* FunctionController = GetController(CallableIndex);

		if (FunctionGraph == nullptr || FunctionController == nullptr) continue;

		for (FCallableArgument& Argument : Callable.Arguments) {
			FString CPPType;
			UObject* CPPTypeObject = nullptr;

			/* What the argument is: the bytecode's own spelling of it, when the registry knows the type by it.
			 * (A pin reading it can be a type-blind copy whose recorded resolution is wrong: ChorusPunt's
			 * createFKCtrl clones its TArray<FName> JointNames through an ArrayClone recorded as TArray<bool>.) */
			if (!Argument.TypeString.IsEmpty() && !ResolveTypeString(Argument.TypeString, CPPType, CPPTypeObject)) {
				CPPType.Reset();
				CPPTypeObject = nullptr;
			}

			/* Otherwise from the pins inside the function that read or write it, which are the engine's own
			 * spelling of the type */
			for (const FNode& Node : Nodes) {
				if (!CPPType.IsEmpty()) break;
				if (Node.Callable != CallableIndex || Node.Node == nullptr) continue;

				for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num() && CPPType.IsEmpty(); ++OperandIndex) {
					if (Node.Operands[OperandIndex].Key != MemoryTypeWork) continue;
					if (!Node.Arguments.IsValidIndex(OperandIndex) || Node.Arguments[OperandIndex].IsEmpty()) continue;

					const int32 Register = Node.Operands[OperandIndex].Value;

					if (Register != Argument.InterfaceRegister && Register != Argument.ForwardedRegister) continue;

					if (const URigVMPin* Pin = Node.Node->FindPin(Node.Arguments[OperandIndex])) {
						CPPType = Pin->GetCPPType();
						CPPTypeObject = Pin->GetCPPTypeObject();
					}
				}

				if (!CPPType.IsEmpty()) break;
			}

			if (CPPType.IsEmpty()) {
				Stats.UntypedArguments++;

				continue;
			}

			const ERigVMPinDirection Direction = Argument.bInput && Argument.bOutput
				? ERigVMPinDirection::IO
				: Argument.bOutput ? ERigVMPinDirection::Output : ERigVMPinDirection::Input;

			const FName PinName = FunctionController->AddExposedPin(
				FName(*Argument.Name),
				Direction,
				CPPType,
				CPPTypeObject != nullptr ? FName(*CPPTypeObject->GetPathName()) : NAME_None,
				FString(),
				false,
				false
			);

			if (PinName.IsNone()) {
				Stats.UntypedArguments++;

				continue;
			}

			Argument.PinName = PinName.ToString();
		}

		/* The entry node writes every input just before the body runs, and the return node reads
		 * every output just after. Both registers are named: the body reads the forwarded one, and
		 * copies reach the interface one. */
		auto AddInterfaceNode = [this, &Callable, CallableIndex](URigVMNode* InterfaceNode, const int32 Instruction, const bool bEntry) -> int32 {
			if (InterfaceNode == nullptr) {
				return INDEX_NONE;
			}

			FNode Node;
			Node.Instruction = Instruction;
			Node.Callable = CallableIndex;
			Node.Name = InterfaceNode->GetName();
			Node.Node = InterfaceNode;

			for (const FCallableArgument& Argument : Callable.Arguments) {
				if (Argument.PinName.IsEmpty()) continue;
				if (bEntry ? !Argument.bInput : !Argument.bOutput) continue;

				for (const int32 Register : { Argument.InterfaceRegister, Argument.ForwardedRegister }) {
					if (Register == INDEX_NONE) continue;

					Node.Operands.Add(TPair<int32, int32>(MemoryTypeWork, Register));
					Node.Arguments.Add(Argument.PinName);
					Node.ArgumentIsOutput.Add(bEntry);
				}
			}

			return Nodes.Add(Node);
		};

		Callable.EntryNode = AddInterfaceNode(FunctionGraph->GetEntryNode(), Callable.First - 1, true);
		Callable.ReturnNode = AddInterfaceNode(FunctionGraph->GetReturnNode(), Callable.Last + 1, false);

		/* An input's registers hold the caller's value and nothing else's, so they are the entry's
		 * whatever their names say */
		if (Callable.EntryNode == INDEX_NONE) continue;

		for (const FCallableArgument& Argument : Callable.Arguments) {
			if (Argument.PinName.IsEmpty() || !Argument.bInput) continue;

			for (const int32 RegisterIndex : { Argument.InterfaceRegister, Argument.ForwardedRegister }) {
				if (FRegister* Register = FindRegister(MemoryTypeWork, RegisterIndex)) {
					Register->Owner = Callable.EntryNode;
					Register->OwnerPin = Argument.PinName;
				}
			}
		}
	}
}

void FRigVMGraphReconstruction::CreateFunctionReferences() {
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		if (Node.Invokes == INDEX_NONE || !Callables.IsValidIndex(Node.Invokes)) continue;

		const FCallable& Callable = Callables[Node.Invokes];

		if (Callable.Function == nullptr) continue;

		const FVector2D Position(400.0f * static_cast<float>(NodeIndex / 12), 260.0f * static_cast<float>(NodeIndex % 12));

		Node.Node = GetController(Node.Callable)->AddFunctionReferenceNode(Callable.Function, Position, Node.Name, false, false);

		if (Node.Node == nullptr) {
			Stats.FailedNodes++;

			continue;
		}

		Stats.Nodes++;
		Stats.FunctionReferences++;

		/* The operands are the function's arguments in order, and a reference's pins are named the
		 * way the function's entry and return took them. One that couldn't be typed has no pin. */
		for (int32 OperandIndex = 0; OperandIndex < Node.Arguments.Num(); ++OperandIndex) {
			const FString PinName = Callable.Arguments.IsValidIndex(OperandIndex) ? Callable.Arguments[OperandIndex].PinName : FString();

			if (FRegister* Register = FindRegister(Node.Operands[OperandIndex].Key, Node.Operands[OperandIndex].Value)) {
				if (Register->Owner == NodeIndex && Register->OwnerPin == Node.Arguments[OperandIndex]) {
					Register->OwnerPin = PinName;
				}
			}

			Node.Arguments[OperandIndex] = PinName;
		}
	}
}

/* One field out of a value, named the way a register offset names it: dot separated, an object's
 * field or an array's index per part */
static TSharedPtr<FJsonValue> ResolveJsonSegment(const TSharedPtr<FJsonValue>& Value, const FString& SegmentPath) {
	TSharedPtr<FJsonValue> Current = Value;

	TArray<FString> Parts;
	SegmentPath.ParseIntoArray(Parts, TEXT("."), true);

	for (const FString& Part : Parts) {
		if (!Current.IsValid()) {
			return nullptr;
		}

		if (Current->Type == EJson::Object) {
			Current = Current->AsObject()->TryGetField(Part);
		} else if (Current->Type == EJson::Array && Part.IsNumeric()) {
			const TArray<TSharedPtr<FJsonValue>>& Elements = Current->AsArray();
			const int32 Index = FCString::Atoi(*Part);

			Current = Elements.IsValidIndex(Index) ? Elements[Index] : nullptr;
		} else {
			return nullptr;
		}
	}

	return Current;
}

FString FRigVMGraphReconstruction::GetSegmentPath(const int32 MemoryType, const int32 RegisterOffset) const {
	if (RegisterOffset == NoRegisterOffset) {
		return FString();
	}

	const TArray<FString>& PropertyPaths = MemoryType == MemoryTypeLiteral ? LiteralPropertyPaths : WorkPropertyPaths;

	return PropertyPaths.IsValidIndex(RegisterOffset) ? PropertyPaths[RegisterOffset] : FString();
}

/* The CPP type a register's property holds (TArray<FName> for an ArrayProperty of NameProperty), or nothing for
 * one this doesn't spell */
FString FRigVMGraphReconstruction::RegisterCPPType(const FRegister& Register) {
	auto Spell = [](const TSharedPtr<FJsonObject>& Tag, auto& Self) -> FString {
		if (!Tag.IsValid()) return FString();

		const FString Type = Tag->GetStringField(TEXT("Type"));

		static const TMap<FString, FString> Plain = {
			{ TEXT("BoolProperty"), TEXT("bool") }, { TEXT("IntProperty"), TEXT("int32") }, { TEXT("FloatProperty"), TEXT("float") },
			{ TEXT("DoubleProperty"), TEXT("double") }, { TEXT("NameProperty"), TEXT("FName") }, { TEXT("StrProperty"), TEXT("FString") },
		};

		if (const FString* Known = Plain.Find(Type)) return *Known;

		if (Type == TEXT("StructProperty")) {
			FString Struct;
			return Tag->TryGetStringField(TEXT("StructType"), Struct) && !Struct.IsEmpty() ? TEXT("F") + Struct : FString();
		}

		if (Type == TEXT("ArrayProperty")) {
			const TSharedPtr<FJsonObject>* Inner = nullptr;
			const FString Element = Tag->TryGetObjectField(TEXT("InnerTypeData"), Inner) ? Self(*Inner, Self) : FString();
			return Element.IsEmpty() ? FString() : FString::Printf(TEXT("TArray<%s>"), *Element);
		}

		return FString();
	};

	return Spell(Register.TagData, Spell);
}

FRigVMGraphReconstruction::FRegister* FRigVMGraphReconstruction::FindRegister(const int32 MemoryType, const int32 RegisterIndex) {
	if (MemoryType == MemoryTypeWork) {
		return Work.IsValidIndex(RegisterIndex) ? &Work[RegisterIndex] : nullptr;
	}

	if (MemoryType == MemoryTypeLiteral) {
		return Literals.IsValidIndex(RegisterIndex) ? &Literals[RegisterIndex] : nullptr;
	}

	return nullptr;
}

/* Everything in front of the node is the graph it was compiled from, spelled as the callstack that
 * reached it. Both are separated from the node by the run of underscores the pin path's own
 * separators were turned into. */
static FString StripGraphPrefix(const FString& InBody) {
	FString Body = InBody;

	int32 Separator = INDEX_NONE;

	for (int32 Index = Body.Len() - 3; Index >= 0; --Index) {
		if (Body[Index] == TEXT('_') && Body[Index + 1] == TEXT('_') && Body[Index + 2] == TEXT('_')) {
			Separator = Index;

			break;
		}
	}

	if (Separator != INDEX_NONE) {
		Body = Body.RightChop(Separator + 3);

		/* A deeper callstack leaves more of them than the three a single separator makes */
		while (Body.StartsWith(TEXT("_"))) {
			Body = Body.RightChop(1);
		}
	}

	return Body;
}

/* A register's name is the pin path it was made for with everything that isn't a letter or a digit
 * turned into an underscore, which is what an array element's index arrives as */
static FString SanitizePinName(const FString& PinName) {
	FString Sanitized = PinName;

	for (int32 Index = 0; Index < Sanitized.Len(); ++Index) {
		const TCHAR Character = Sanitized[Index];

		if (!FChar::IsAlpha(Character) && !(Index > 0 && FChar::IsDigit(Character)) && Character != TEXT('_')) {
			Sanitized[Index] = TEXT('_');
		}
	}

	return Sanitized;
}

static FString StripRegisterSuffix(const FString& RegisterName) {
	FString Body = RegisterName;

	/* A literal, and an input nothing is linked into, are marked as what they are */
	if (Body.EndsWith(TEXT("__Const"))) {
		LeftInline(Body, Body.Len() - 7);
	} else if (Body.EndsWith(TEXT("__IO"))) {
		LeftInline(Body, Body.Len() - 4);
	}

	return Body;
}

bool FRigVMGraphReconstruction::SplitRegisterName(const FString& RegisterName, const FString& PinName, FString& OutNodeName) {
	if (PinName.IsEmpty()) {
		return false;
	}

	FString Body = StripRegisterSuffix(RegisterName);

	const FString Suffix = TEXT("_") + SanitizePinName(PinName);

	if (!Body.EndsWith(Suffix, ESearchCase::CaseSensitive)) {
		return false;
	}

	LeftInline(Body, Body.Len() - Suffix.Len());

	Body = StripGraphPrefix(Body);

	if (Body.IsEmpty()) {
		return false;
	}

	OutNodeName = Body;

	return true;
}

bool FRigVMGraphReconstruction::SplitRegisterTail(const FString& RegisterName, FString& OutNodeName, FString& OutPinName) {
	const FString Body = StripGraphPrefix(StripRegisterSuffix(RegisterName));

	FString NodeName;
	FString PinName;

	if (!Body.Split(TEXT("_"), &NodeName, &PinName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
		return false;
	}

	if (NodeName.IsEmpty() || PinName.IsEmpty()) {
		return false;
	}

	OutNodeName = NodeName;
	OutPinName = PinName;

	return true;
}

void FRigVMGraphReconstruction::CollectNodes(const TArray<FUObjectJsonValueExport>& Instructions, const TArray<FString>& FunctionNames) {
	const auto& Registry = GetRigVMRegistry();

	/* How many execute instructions touch each work register. A register several of them share
	 * carries a value between nodes, and reading a node's own name off one would call the node
	 * after its neighbour. */
	TMap<int32, int32> WorkRegisterUses;

	for (const FUObjectJsonValueExport& Instruction : Instructions) {
		const int32 OpCode = Instruction.GetInteger(TEXT("OpCode"), -1);

		if (OpCode != OpCodeExecute && OpCode != OpCodeInvokeCallable) continue;
		if (!Instruction.Has(TEXT("Arguments"))) continue;

		for (const FUObjectJsonValueExport& Argument : Instruction.GetArray(TEXT("Arguments"))) {
			if (Argument.GetInteger(TEXT("MemoryType"), INDEX_NONE) != MemoryTypeWork) continue;

			WorkRegisterUses.FindOrAdd(Argument.GetInteger(TEXT("RegisterIndex"), INDEX_NONE))++;
		}
	}

	/* The node is whichever one the registers are named after. An output is only ever named
	 * after the pin that writes it, so it is asked first; an input that nothing is linked into
	 * has a register of its own too, and a literal is the last word because the compiler shares
	 * one between every pin that was authored with the same value. */
	auto AddNode = [this, &WorkRegisterUses](FNode& Node, const FString& FallbackName) {
		FString NodeName;

		for (int32 Pass = 0; Pass < 3 && NodeName.IsEmpty(); ++Pass) {
			for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
				const int32 MemoryType = Node.Operands[OperandIndex].Key;
				const bool bIsLiteral = MemoryType == MemoryTypeLiteral;

				/* Inputs only name the node when nothing else shares the register */
				if (Pass == 1 && MemoryType == MemoryTypeWork && !Node.ArgumentIsOutput[OperandIndex]) {
					if (const int32* Uses = WorkRegisterUses.Find(Node.Operands[OperandIndex].Value)) {
						if (*Uses > 1) continue;
					}
				}

				/* An argument is only ever const or mutable here: what the graph calls an IO pin
				 * is a mutable one, and is written the same way an output is */
				const bool bIsOutput = Node.ArgumentIsOutput[OperandIndex];

				if (Pass == 0 && (bIsLiteral || !bIsOutput)) continue;
				if (Pass == 1 && bIsLiteral) continue;
				if (Pass == 2 && !bIsLiteral) continue;

				const FRegister* Register = FindRegister(MemoryType, Node.Operands[OperandIndex].Value);
				if (Register == nullptr) continue;

				if (SplitRegisterName(Register->Name, Node.Arguments[OperandIndex], NodeName)) {
					break;
				}
			}
		}

		if (NodeName.IsEmpty()) {
			NodeName = FallbackName;
		}

		/* Two graphs compiled into the same VM can each hold a node of the same name */
		FString UniqueName = NodeName;

		for (int32 Suffix = 1; NodeNames.Contains(UniqueName); ++Suffix) {
			UniqueName = FString::Printf(TEXT("%s_%d"), *NodeName, Suffix);
		}

		NodeNames.Add(UniqueName);

		Node.Name = UniqueName;
		Node.DerivedName = NodeName;

		const int32 NodeIndex = Nodes.Add(Node);
		InstructionToNode.Add(Node.Instruction, NodeIndex);
	};

	for (int32 Index = 0; Index < Instructions.Num(); ++Index) {
		const FUObjectJsonValueExport& Instruction = Instructions[Index];
		const int32 OpCode = Instruction.GetInteger(TEXT("OpCode"), -1);

		/* A call to one of the functions compiled into the same bytecode. Its operands are the
		 * function's arguments in order, which is all a reference to it needs. */
		if (OpCode == OpCodeInvokeCallable) {
			const int32 CallableIndex = Instruction.GetInteger(TEXT("CallableIndex"), INDEX_NONE);

			if (!Callables.IsValidIndex(CallableIndex)) continue;

			const FCallable& Callable = Callables[CallableIndex];

			FNode Node;
			Node.Instruction = Index;
			Node.Callable = FindCallable(Index);
			Node.Invokes = CallableIndex;

			const TArray<FUObjectJsonValueExport> Arguments = Instruction.Has(TEXT("Arguments"))
				? Instruction.GetArray(TEXT("Arguments"))
				: TArray<FUObjectJsonValueExport>();

			for (int32 ArgumentIndex = 0; ArgumentIndex < Arguments.Num(); ++ArgumentIndex) {
				Node.Operands.Add(TPair<int32, int32>(
					Arguments[ArgumentIndex].GetInteger(TEXT("MemoryType"), INDEX_NONE),
					Arguments[ArgumentIndex].GetInteger(TEXT("RegisterIndex"), INDEX_NONE)
				));

				const bool bKnown = Callable.Arguments.IsValidIndex(ArgumentIndex);

				Node.Arguments.Add(bKnown ? Callable.Arguments[ArgumentIndex].Name : FString());
				Node.ArgumentIsOutput.Add(bKnown && Callable.Arguments[ArgumentIndex].bOutput);
			}

			AddNode(Node, Callable.Name);

			continue;
		}

		if (OpCode != OpCodeExecute) {
			continue;
		}

		const int32 FunctionIndex = Instruction.GetInteger(TEXT("FunctionIndex"), INDEX_NONE);

		if (!FunctionNames.IsValidIndex(FunctionIndex)) {
			continue;
		}

		const FString& FunctionName = FunctionNames[FunctionIndex];

		/* A sequence compiles into nothing: its branches are flattened into one run and the branch
		 * table keeps no record of where one ended and the next began. The node the editor shows
		 * for it would sit in the chain with every pin but the first unused, so execution chains
		 * straight through instead. */
		if (FunctionName.StartsWith(TEXT("FRigVMFunction_Sequence::"))) {
			continue;
		}

		const FRigVMFunction* Function = Registry.FindFunction(*FunctionName);

		/* A rig built against a plugin this project doesn't have, or a unit an engine this old
		 * never had, is a node that cannot be made rather than one to guess at */
		if (Function == nullptr) {
			Stats.MissingFunctions.AddUnique(FunctionName);

			/* A function whose body runs it does nothing a call to it relies on */
			const int32 Callable = FindCallable(Index);

			if (Callables.IsValidIndex(Callable)) {
				Stats.UnresolvedFunctions.AddUnique(FString::Printf(TEXT("%s (%s)"), *Callables[Callable].Name, *FunctionName));
			}

			continue;
		}

		FNode Node;
		Node.Instruction = Index;
		Node.Function = Function;
		Node.Callable = FindCallable(Index);

		const TArray<FUObjectJsonValueExport> Arguments = Instruction.Has(TEXT("Arguments"))
			? Instruction.GetArray(TEXT("Arguments"))
			: TArray<FUObjectJsonValueExport>();

		for (const FUObjectJsonValueExport& Argument : Arguments) {
			Node.Operands.Add(TPair<int32, int32>(
				Argument.GetInteger(TEXT("MemoryType"), INDEX_NONE),
				Argument.GetInteger(TEXT("RegisterIndex"), INDEX_NONE)
			));
		}

		/* The operands are the function's arguments in its own order, which is what turns a
		 * register back into the pin it was made for */
		const TArray<FRigVMFunctionArgument>& FunctionArguments = Function->GetArguments();

		/* The registry's argument list doesn't say which way an argument goes, so the direction
		 * comes from where it is actually declared: the unit struct's own property markup, or the
		 * dispatch's argument infos. An IO pin writes its register the same way an output does. */
		auto IsOutputArgument = [Function](const FString& ArgumentName) -> bool {
			if (Function->Struct != nullptr) {
				if (const FProperty* Property = Function->Struct->FindPropertyByName(*ArgumentName)) {
					return Property->HasMetaData(TEXT("Output"));
				}

				return false;
			}

			if (Function->Factory != nullptr) {
				for (const FDispatchArgument& Info : GetFactoryArgumentInfos(Function->Factory)) {
					if (Info.Name.ToString() == ArgumentName) {
						return Info.Direction == ERigVMPinDirection::Output || Info.Direction == ERigVMPinDirection::IO;
					}
				}
			}

			return false;
		};

		if (FunctionArguments.Num() == Node.Operands.Num()) {
			for (const FRigVMFunctionArgument& Argument : FunctionArguments) {
				Node.Arguments.Add(Argument.Name);
				Node.ArgumentIsOutput.Add(IsOutputArgument(Argument.Name));
			}
		} else if (Function->Factory != nullptr) {
			/* A dispatch that takes as many operands as it was given values. The registry's own
			 * labelling of these is written for the runtime's handle layout, which carries one
			 * entry the serialized operands don't, and spells element pins with an underscore
			 * where the pin path takes a dot: both together put every value on a pin that doesn't
			 * exist. The shapes are the dispatch's own, so they are spelled out here instead. */
			const int32 OperandCount = Node.Operands.Num();

			if (FunctionName.Contains(TEXT("RigVMDispatch_ArrayMake"))) {
				/* Every element in authoring order, then the array they make */
				for (int32 OperandIndex = 0; OperandIndex < OperandCount - 1; ++OperandIndex) {
					Node.Arguments.Add(FString::Printf(TEXT("Values.%d"), OperandIndex));
					Node.ArgumentIsOutput.Add(false);
				}

				Node.Arguments.Add(TEXT("Array"));
				Node.ArgumentIsOutput.Add(true);
			} else if (FunctionName.Contains(TEXT("RigVMDispatch_Select"))) {
				/* The index, one value per case, and the value picked out */
				Node.Arguments.Add(TEXT("Index"));
				Node.ArgumentIsOutput.Add(false);

				for (int32 OperandIndex = 1; OperandIndex < OperandCount - 1; ++OperandIndex) {
					Node.Arguments.Add(FString::Printf(TEXT("Values.%d"), OperandIndex - 1));
					Node.ArgumentIsOutput.Add(false);
				}

				Node.Arguments.Add(TEXT("Result"));
				Node.ArgumentIsOutput.Add(true);
			} else {
				for (int32 OperandIndex = 0; OperandIndex < OperandCount; ++OperandIndex) {
					const FString ArgumentName = GetFunctionArgumentName(Function, OperandIndex, OperandCount).ToString();

					Node.Arguments.Add(ArgumentName);
					Node.ArgumentIsOutput.Add(IsOutputArgument(ArgumentName));
				}
			}
		} else {
			/* The same node, with pins the game's engine gave it and this one didn't.
			 *
			 * The operands are still in the order the node declares them, so all that is missing is
			 * where the extra ones sit. Reading a register's own name back tells whether an operand
			 * lines up with the argument it was handed, which is enough to look for the alignment
			 * that agrees with the most of them: everything before and after the extra pin then
			 * lands on the pin it belongs to, links included. */
			Stats.MismatchedFunctions.AddUnique(FunctionName);

			const int32 Extra = Node.Operands.Num() - FunctionArguments.Num();

			TArray<int32> Alignment;
			int32 BestScore = -1;

			auto ScoreAlignment = [this, &Node, &FunctionArguments](const int32 SkipAt) -> TPair<int32, TArray<int32>> {
				TArray<int32> Mapping;
				Mapping.Reserve(Node.Operands.Num());

				int32 Score = 0;
				int32 ArgumentIndex = 0;

				for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
					if (OperandIndex == SkipAt || !FunctionArguments.IsValidIndex(ArgumentIndex)) {
						Mapping.Add(INDEX_NONE);

						continue;
					}

					Mapping.Add(ArgumentIndex);

					const FRegister* Register = FindRegister(Node.Operands[OperandIndex].Key, Node.Operands[OperandIndex].Value);

					FString OwnerName;

					if (Register != nullptr && SplitRegisterName(Register->Name, FunctionArguments[ArgumentIndex].Name, OwnerName)) {
						Score++;
					}

					ArgumentIndex++;
				}

				return TPair<int32, TArray<int32>>(Score, Mapping);
			};

			/* Only one pin's worth of difference can be placed this way. More than that and the
			 * operands are read off their own registers instead, which leaves the linked ones out. */
			if (Extra == 1) {
				for (int32 SkipAt = 0; SkipAt < Node.Operands.Num(); ++SkipAt) {
					TPair<int32, TArray<int32>> Scored = ScoreAlignment(SkipAt);

					if (Scored.Key > BestScore) {
						BestScore = Scored.Key;
						Alignment = Scored.Value;
					}
				}
			}

			for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
				int32 ArgumentIndex = INDEX_NONE;

				if (BestScore > 0 && Alignment.IsValidIndex(OperandIndex)) {
					ArgumentIndex = Alignment[OperandIndex];
				} else {
					/* No alignment to go on: the operand is whatever pin its own register is named
					 * after, and one this version doesn't declare has nowhere to land */
					const FRegister* Register = FindRegister(Node.Operands[OperandIndex].Key, Node.Operands[OperandIndex].Value);

					FString RegisterNode;
					FString RegisterPin;

					if (Register != nullptr && SplitRegisterTail(Register->Name, RegisterNode, RegisterPin)) {
						ArgumentIndex = FunctionArguments.IndexOfByPredicate([&RegisterPin](const FRigVMFunctionArgument& Candidate) {
							return RegisterPin.Equals(Candidate.Name, ESearchCase::CaseSensitive);
						});
					}
				}

				if (ArgumentIndex == INDEX_NONE) {
					Stats.DroppedPins++;

					Node.Arguments.Add(FString());
					Node.ArgumentIsOutput.Add(false);

					continue;
				}

				Node.Arguments.Add(FunctionArguments[ArgumentIndex].Name);
				Node.ArgumentIsOutput.Add(IsOutputArgument(FunctionArguments[ArgumentIndex].Name));
			}
		}

		FString StructName = FunctionName;
		StructName.Split(TEXT("::"), &StructName, nullptr);

		AddNode(Node, FString::Printf(TEXT("%s_%d"), *StructName, Index));
	}

	ClaimRegisters();
}

void FRigVMGraphReconstruction::ClaimRegisters() {
	/* How many nodes read or write each work register: a register named after an unlinked input
	 * only its own node touches, and one several instructions share is a value travelling between
	 * them whatever its name says */
	TMap<int32, TSet<int32>> RegisterUsers;

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		for (const TPair<int32, int32>& Operand : Nodes[NodeIndex].Operands) {
			if (Operand.Key != MemoryTypeWork) continue;

			RegisterUsers.FindOrAdd(Operand.Value).Add(NodeIndex);
		}
	}

	/* Every register is named after one pin, and every other pin reading it is linked to that one.
	 *
	 * Written pins go first. Two nodes either side of a link often name the pin the same way, a
	 * spline reaching a node that reads one being the case that shows up, and letting whichever of
	 * them was compiled first take the register names it after the pin that reads it: the link is
	 * then built from the reader to the writer, which the graph refuses. The pin that writes a
	 * register is the one it belongs to, so it is asked before anything that only reads. */
	for (int32 Pass = 0; Pass < 2; ++Pass) {
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
			FNode& Node = Nodes[NodeIndex];

			for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
				if (!Node.Arguments.IsValidIndex(OperandIndex)) continue;

				const bool bIsOutput = Node.ArgumentIsOutput[OperandIndex];

				if (Pass == 0 && !bIsOutput) continue;
				if (Pass == 1 && bIsOutput) continue;

				/* An input pin only owns a register nothing else touches. A shared one is another
				 * node's value on its way in, however this node's pin happens to be named. */
				if (Pass == 1 && Node.Operands[OperandIndex].Key == MemoryTypeWork) {
					if (const TSet<int32>* Users = RegisterUsers.Find(Node.Operands[OperandIndex].Value)) {
						if (Users->Num() > 1) continue;
					}
				}

				FRegister* Register = FindRegister(Node.Operands[OperandIndex].Key, Node.Operands[OperandIndex].Value);

				if (Register == nullptr || Register->Owner != INDEX_NONE) continue;

				FString OwnerName;

				if (SplitRegisterName(Register->Name, Node.Arguments[OperandIndex], OwnerName) && OwnerName == Node.DerivedName) {
					Register->Owner = NodeIndex;
					Register->OwnerPin = Node.Arguments[OperandIndex];
				}
			}
		}
	}

	/* What is left over is a value whose register is named after something that no longer exists:
	 * a local variable of a collapsed graph, or a pin of a node whose name came out differently.
	 * The name cannot say whose it is, but the direction still can: the node that writes it is
	 * where the value comes from, and every reader belongs linked to that pin. */
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
			if (!Node.Arguments.IsValidIndex(OperandIndex)) continue;
			if (!Node.ArgumentIsOutput[OperandIndex]) continue;
			if (Node.Arguments[OperandIndex].IsEmpty()) continue;
			if (Node.Operands[OperandIndex].Key != MemoryTypeWork) continue;

			FRegister* Register = FindRegister(MemoryTypeWork, Node.Operands[OperandIndex].Value);

			if (Register == nullptr || Register->Owner != INDEX_NONE) continue;

			Register->Owner = NodeIndex;
			Register->OwnerPin = Node.Arguments[OperandIndex];
		}
	}
}

void FRigVMGraphReconstruction::CreateNodes() {
	const auto& Registry = GetRigVMRegistry();

	for (int32 Index = 0; Index < Nodes.Num(); ++Index) {
		FNode& Node = Nodes[Index];

		/* A call is made once the function it calls has its pins */
		if (Node.Function == nullptr) continue;

		/* A function's body goes in the function's own graph */
		URigVMController* GraphController = GetController(Node.Callable);

		/* Laid out in the order the VM runs them, which is the only order a cooked rig still has */
		const FVector2D Position(400.0f * static_cast<float>(Index / 12), 260.0f * static_cast<float>(Index % 12));

		if (Node.Function->Struct != nullptr) {
			FString MethodName = Node.Function->Name;
			MethodName.Split(TEXT("::"), nullptr, &MethodName);

			Node.Node = GraphController->AddUnitNode(
				Node.Function->Struct,
				MethodName.IsEmpty() ? FName(TEXT("Execute")) : FName(*MethodName),
				Position,
				Node.Name,
				false,
				false
			);
		} else if (Node.Function->Factory != nullptr) {
			Node.Node = GraphController->AddTemplateNode(
				Node.Function->Factory->GetTemplateNotation(),
				Position,
				Node.Name,
				false,
				false
			);
		}

		if (Node.Node == nullptr) {
			Stats.FailedNodes++;

			continue;
		}

		Stats.Nodes++;

		/* A dispatch node comes in as the template it is, with its pins still wildcards. The
		 * function the instruction named is one resolution of that template, and its argument
		 * types are what the pins were resolved to when the rig was compiled. */
		if (Node.Function->Factory != nullptr) {
			const TArray<FRigVMFunctionArgument>& Arguments = Node.Function->GetArguments();
			const TArray<TRigVMTypeIndex>& TypeIndices = Node.Function->GetArgumentTypeIndices();

			/* A clone copies memory without looking at it, and the cook can record it under any array type
			 * (ChorusPunt's TArray<FName> local set through an ArrayClone recorded as TArray<bool>): its type is
			 * the one the registers it copies hold */
			TOptional<FRigVMTemplateArgumentType> CloneType;

			if (Node.Function->Factory->GetFactoryName().ToString().Contains(TEXT("ArrayClone"))) {
				for (const TPair<int32, int32>& Operand : Node.Operands) {
					if (Operand.Key != MemoryTypeWork) continue;

					/* A function argument it copies says its type outright (an array of a user-defined struct,
					 * which the register's property can't name); otherwise the register's property */
					FString CPPType;

					if (Callables.IsValidIndex(Node.Callable)) {
						for (const FCallableArgument& Argument : Callables[Node.Callable].Arguments) {
							if (Operand.Value == Argument.InterfaceRegister || Operand.Value == Argument.ForwardedRegister) CPPType = Argument.TypeString;
						}
					}

					if (CPPType.IsEmpty()) {
						const FRegister* Register = FindRegister(MemoryTypeWork, Operand.Value);
						CPPType = Register != nullptr ? RegisterCPPType(*Register) : FString();
					}

					FString Resolved;
					UObject* ResolvedObject = nullptr;

					if (CPPType.StartsWith(TEXT("TArray<")) && ResolveTypeString(CPPType, Resolved, ResolvedObject)) {
						CloneType = FRigVMTemplateArgumentType(FName(*Resolved), ResolvedObject);
						break;
					}
				}
			}

			for (int32 ArgumentIndex = 0; ArgumentIndex < Arguments.Num(); ++ArgumentIndex) {
				if (!TypeIndices.IsValidIndex(ArgumentIndex)) continue;

				URigVMPin* Pin = Node.Node->FindPin(Arguments[ArgumentIndex].Name);

				if (Pin == nullptr || !Pin->IsWildCard()) continue;

				GraphController->ResolveWildCardPin(Pin, CloneType.IsSet() ? CloneType.GetValue() : Registry.GetType(TypeIndices[ArgumentIndex]), false, false);
			}
		}

		/* A node that takes as many values as it was given, an array made out of its elements being
		 * the one this reaches, names each operand for an element of an array pin. Those elements
		 * only exist once the pin has been grown to hold them, and until it has there is nothing
		 * for all but the first of them to link to. */
		TMap<FString, int32> ArraySizes;

		for (const FString& Argument : Node.Arguments) {
			FString ArrayPinName;
			FString ElementIndex;

			if (!Argument.Split(TEXT("."), &ArrayPinName, &ElementIndex, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) continue;
			if (!ElementIndex.IsNumeric()) continue;

			int32& Size = ArraySizes.FindOrAdd(ArrayPinName);
			Size = FMath::Max(Size, FCString::Atoi(*ElementIndex) + 1);
		}

		for (const TPair<FString, int32>& ArraySize : ArraySizes) {
			/* An output array (ArrayClone's Clone) gets its elements when the VM runs; the controller refuses to
			 * insert any under it ("Cannot insert array pin under ..."), so only inputs are grown. */
			if (const URigVMPin* ArrayPin = Node.Node->FindPin(ArraySize.Key); ArrayPin && ArrayPin->GetDirection() != ERigVMPinDirection::Output) {
				GraphController->SetArrayPinSize(ArrayPin->GetPinPath(), ArraySize.Value, FString(), false, false);
			}
		}
	}
}

bool FRigVMGraphReconstruction::SetPinDefault(URigVMPin* Pin, const TSharedPtr<FJsonValue>& Value) {
	if (Pin == nullptr || !Value.IsValid() || Value->IsNull()) {
		return false;
	}

	if (Value->Type == EJson::Array) {
		const TArray<TSharedPtr<FJsonValue>>& Elements = Value->AsArray();

		/* A register can hold an array where the pin this engine build declares holds one value; an output
		 * array's elements are the VM's to make, not a default */
		if (!Pin->IsArray() || Pin->GetDirection() == ERigVMPinDirection::Output) {
			return false;
		}

		/* Resizing to the size the pin already has reports nothing done, which a one element array
		 * reaching a pin that defaults to one element is */
		if (Pin->GetArraySize() != Elements.Num() && !GetController(Pin->GetNode())->SetArrayPinSize(Pin->GetPinPath(), Elements.Num(), FString(), false, false)) {
			return false;
		}

		bool bAny = false;

		for (int32 Index = 0; Index < Elements.Num(); ++Index) {
			if (URigVMPin* ElementPin = Pin->FindSubPin(FString::FromInt(Index))) {
				bAny |= SetPinDefault(ElementPin, Elements[Index]);
			}
		}

		return bAny;
	}

	if (Value->Type == EJson::Object) {
		const TSharedPtr<FJsonObject> Object = Value->AsObject();

		/* The maths types draw as one widget rather than a tree: a quaternion or a vector pin has
		 * no sub pins to walk, and the whole value has to arrive as one string. The struct itself
		 * knows how to read the json and how to write that string, so it does both. */
		if (Pin->GetSubPins().Num() == 0) {
			UScriptStruct* Struct = Cast<UScriptStruct>(Pin->GetCPPTypeObject());

			if (Struct == nullptr) {
				return false;
			}

			TArray<uint8> Memory;
			Memory.SetNumZeroed(Struct->GetStructureSize());

			Struct->InitializeStruct(Memory.GetData());

			PropertySerializer->DeserializeStruct(Struct, Object.ToSharedRef(), Memory.GetData());

			FString Exported;
			Struct->ExportText(Exported, Memory.GetData(), nullptr, nullptr, PPF_None, nullptr);

			Struct->DestroyStruct(Memory.GetData());

			if (Exported.IsEmpty() || Exported == TEXT("()")) {
				return false;
			}

			return RigSetPinDefault(GetController(Pin->GetNode()), Pin->GetPinPath(), Exported);
		}

		/* Walked pin first rather than field first: an exporter writes what it read off the struct,
		 * including the members that are cached rather than stored, and a pin that has no such
		 * field would refuse the value as a whole */
		bool bAny = false;

		for (URigVMPin* SubPin : Pin->GetSubPins()) {
			const TSharedPtr<FJsonValue> Field = Object->TryGetField(SubPin->GetName());

			if (Field.IsValid()) {
				bAny |= SetPinDefault(SubPin, Field);
			}
		}

		return bAny;
	}

	const FString DefaultValue = ToDefaultValue(Value, Pin);

	if (DefaultValue.IsEmpty()) {
		return false;
	}

	return RigSetPinDefault(GetController(Pin->GetNode()), Pin->GetPinPath(), DefaultValue);
}

void FRigVMGraphReconstruction::UpdateLayout(URigVMGraph* InGraph) {
	if (InGraph == nullptr) {
		return;
	}

	URigVMController* LayoutController = Blueprint->GetOrCreateController(InGraph);

	const TArray<URigVMNode*> GraphNodes = InGraph->GetNodes();

	/* Left to right: a node sits one column right of everything feeding it, so values flow the way
	 * the editor reads and the execution spine runs across the page */
	TMap<URigVMNode*, TArray<URigVMNode*>> Sources;
	TMap<URigVMNode*, int32> PendingSources;

	for (URigVMNode* Node : GraphNodes) {
		PendingSources.Add(Node, 0);
	}

	for (const URigVMLink* GraphLink : InGraph->GetLinks()) {
		if (GraphLink->GetSourcePin() == nullptr || GraphLink->GetTargetPin() == nullptr) continue;

		URigVMNode* Source = GraphLink->GetSourcePin()->GetNode();
		URigVMNode* Target = GraphLink->GetTargetPin()->GetNode();

		if (Source == nullptr || Target == nullptr || Source == Target) continue;

		Sources.FindOrAdd(Target).AddUnique(Source);
	}

	for (const TPair<URigVMNode*, TArray<URigVMNode*>>& Pair : Sources) {
		PendingSources.FindOrAdd(Pair.Key) = Pair.Value.Num();
	}

	/* Longest path from the nodes nothing feeds, walked topologically */
	TMap<URigVMNode*, int32> Columns;

	TArray<URigVMNode*> Ready;

	for (URigVMNode* Node : GraphNodes) {
		if (PendingSources.FindChecked(Node) == 0) {
			Ready.Add(Node);
			Columns.Add(Node, 0);
		}
	}

	TMap<URigVMNode*, TArray<URigVMNode*>> Targets;

	for (const TPair<URigVMNode*, TArray<URigVMNode*>>& Pair : Sources) {
		for (URigVMNode* Source : Pair.Value) {
			Targets.FindOrAdd(Source).Add(Pair.Key);
		}
	}

	for (int32 Index = 0; Index < Ready.Num(); ++Index) {
		URigVMNode* Node = Ready[Index];

		if (const TArray<URigVMNode*>* NodeTargets = Targets.Find(Node)) {
			for (URigVMNode* Target : *NodeTargets) {
				Columns.FindOrAdd(Target) = FMath::Max(Columns.FindOrAdd(Target), Columns.FindChecked(Node) + 1);

				if (--PendingSources.FindChecked(Target) == 0) {
					Ready.Add(Target);
				}
			}
		}
	}

	/* Feeding forward puts every value node as far left as it can go, which for one with no inputs
	 * of its own is the far edge of the graph however deep its reader sits. The execution spine
	 * keeps the columns the walk gave it, and everything pure is pulled right instead until it
	 * meets its first reader, so a value sits beside the node that uses it. Walked in reverse
	 * order so a feeder of a feeder follows the node it was pulled toward. */
	for (int32 Index = Ready.Num() - 1; Index >= 0; --Index) {
		URigVMNode* Node = Ready[Index];

		if (Node->IsMutable()) continue;

		const TArray<URigVMNode*>* NodeTargets = Targets.Find(Node);

		if (NodeTargets == nullptr || NodeTargets->Num() == 0) continue;

		int32 Earliest = MAX_int32;

		for (URigVMNode* Target : *NodeTargets) {
			Earliest = FMath::Min(Earliest, Columns.FindOrAdd(Target));
		}

		if (Earliest != MAX_int32) {
			int32& Column = Columns.FindChecked(Node);
			Column = FMath::Max(Column, Earliest - 1);
		}
	}

	/* The graph refuses cyclic links, so anything not reached would only be a node with no links
	 * at all, which the loop above already seeded at column zero */

	/* Stack each column top down, in the order the sources sit so links cross less, leaving each
	 * node the room its pins take up */
	TMap<int32, TArray<URigVMNode*>> ByColumn;
	int32 LastColumn = 0;

	for (URigVMNode* Node : GraphNodes) {
		const int32 Column = Columns.Contains(Node) ? Columns.FindChecked(Node) : 0;

		ByColumn.FindOrAdd(Column).Add(Node);
		LastColumn = FMath::Max(LastColumn, Column);
	}

	TMap<URigVMNode*, float> Rows;

	for (int32 Column = 0; Column <= LastColumn; ++Column) {
		TArray<URigVMNode*>* ColumnNodes = ByColumn.Find(Column);
		if (ColumnNodes == nullptr) continue;

		/* Where a node's sources sit decides where it wants to be */
		ColumnNodes->StableSort([&Sources, &Rows](URigVMNode& A, URigVMNode& B) {
			auto Average = [&Sources, &Rows](URigVMNode& Node) -> float {
				const TArray<URigVMNode*>* NodeSources = Sources.Find(&Node);

				if (NodeSources == nullptr || NodeSources->Num() == 0) return 0.0f;

				float Sum = 0.0f;
				int32 Count = 0;

				for (URigVMNode* Source : *NodeSources) {
					if (const float* Row = Rows.Find(Source)) {
						Sum += *Row;
						Count++;
					}
				}

				return Count > 0 ? Sum / static_cast<float>(Count) : 0.0f;
			};

			return Average(A) < Average(B);
		});

		float Row = 0.0f;

		for (URigVMNode* Node : *ColumnNodes) {
			LayoutController->SetNodePosition(Node, FVector2D(480.0f * static_cast<float>(Column), Row), false, false, false);

			Rows.Add(Node, Row);

			/* The next node goes below everything this one draws */
			Row += 120.0f + 24.0f * static_cast<float>(Node->GetPins().Num());
		}
	}
}

void FRigVMGraphReconstruction::PromoteLocalVariables() {
	/* A register named after a local variable of a collapsed graph has one loose end left: the
	 * first node writing it reads whatever the local held, and the local no longer exists to be
	 * read. It comes back as a variable on the rig itself, so the pin gets its getter and the
	 * value has somewhere to live.
	 *
	 * Last of all because making the variable compiles the blueprint, and a compile is licensed
	 * to collect garbage: everything the reconstruction still wanted, the import's serializers
	 * included, has to be done needing it first. */
	struct FLoose {
		FString VariableName;
		URigVMPin* Pin = nullptr;
	};

	TArray<FLoose> LooseEnds;

	TMap<int32, int32> FirstToucher;

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		if (Node.Node == nullptr) continue;

		for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
			if (!Node.Arguments.IsValidIndex(OperandIndex)) continue;
			if (Node.Arguments[OperandIndex].IsEmpty()) continue;
			if (Node.Operands[OperandIndex].Key != MemoryTypeWork) continue;

			const int32 RegisterIndex = Node.Operands[OperandIndex].Value;

			const FRegister* Register = FindRegister(MemoryTypeWork, RegisterIndex);

			if (Register == nullptr || !Register->Name.StartsWith(TEXT("LocalVariable"))) continue;

			const int32* Existing = FirstToucher.Find(RegisterIndex);

			if (Existing != nullptr && Nodes[*Existing].Instruction <= Node.Instruction) continue;

			URigVMPin* Pin = Node.Node->FindPin(Node.Arguments[OperandIndex]);

			if (Pin == nullptr || Pin->GetDirection() != ERigVMPinDirection::IO) continue;
			if (Pin->GetSourceLinks(true).Num() > 0) continue;

			/* LocalVariable__SplineIK_offset_positions -> SplineIK_offset_positions */
			FString VariableName = Register->Name.RightChop(13);

			while (VariableName.StartsWith(TEXT("_"))) {
				VariableName = VariableName.RightChop(1);
			}

			/* Inside the function it belongs to, the variable is one of the function's own, named
			 * without the function in front: SplineIK_offset_positions -> offset_positions */
			if (Callables.IsValidIndex(Node.Callable)) {
				const FString FunctionPrefix = Callables[Node.Callable].Name + TEXT("_");

				if (VariableName.StartsWith(FunctionPrefix, ESearchCase::CaseSensitive) && VariableName.Len() > FunctionPrefix.Len()) {
					VariableName = VariableName.RightChop(FunctionPrefix.Len());
				}
			}

			if (VariableName.IsEmpty()) continue;

			FirstToucher.Add(RegisterIndex, NodeIndex);

			LooseEnds.Add({ VariableName, Pin });
		}
	}

	for (const FLoose& Loose : LooseEnds) {
		if (Loose.Pin->GetSourceLinks(true).Num() > 0) continue;

		URigVMController* PinController = GetController(Loose.Pin->GetNode());
		URigVMGraph* PinGraph = Loose.Pin->GetGraph();

		/* A function keeps its variables to itself, as locals */
		const bool bInFunction = PinGraph != nullptr && PinGraph != Graph;

		const TArray<FRigVMGraphVariableDescription> Existing = bInFunction ? PinGraph->GetLocalVariables() : Blueprint->GetMemberVariables();

		const bool bExists = Existing.ContainsByPredicate([&Loose](const FRigVMGraphVariableDescription& Description) {
			return Description.Name.ToString() == Loose.VariableName;
		});

		if (!bExists && bInFunction) {
			const FRigVMGraphVariableDescription Local = PinController->AddLocalVariable(
				FName(*Loose.VariableName),
				Loose.Pin->GetCPPType(),
				Loose.Pin->GetCPPTypeObject(),
				Loose.Pin->GetDefaultValue(),
				false,
				false
			);

			if (Local.Name.IsNone()) {
				continue;
			}
		} else if (!bExists) {
			/* The variable API wants the type spelled as an object path rather than as the CPP
			 * type the pin holds, and asserts on anything else it can't name */
			FString TypePath = Loose.Pin->GetCPPType();

			if (const UObject* TypeObject = Loose.Pin->GetCPPTypeObject()) {
				TypePath = TypeObject->GetPathName();

				if (Loose.Pin->IsArray()) {
					TypePath = FString::Printf(TEXT("TArray<%s>"), *TypePath);
				}
			}

			if (Blueprint->AddMemberVariable(FName(*Loose.VariableName), TypePath, false, false, Loose.Pin->GetDefaultValue()).IsNone()) {
				continue;
			}
		}

		const FVector2D Position = Loose.Pin->GetNode()->GetPosition() + FVector2D(-480.0f, 40.0f);

		URigVMNode* Getter = PinController->AddVariableNode(
			FName(*Loose.VariableName),
			Loose.Pin->GetCPPType(),
			Loose.Pin->GetCPPTypeObject(),
			true,
			FString(),
			Position,
			FString(),
			false,
			false
		);

		if (Getter == nullptr) continue;

		URigVMPin* ValuePin = Getter->FindPin(TEXT("Value"));

		if (ValuePin != nullptr && PinController->AddLink(ValuePin->GetPinPath(), Loose.Pin->GetPinPath(), false, false)) {
			Stats.Links++;
			Stats.VariableNodes++;
		} else {
			/* No variable node without its wire */
			PinController->RemoveNode(Getter, false, false);
		}
	}
}

void FRigVMGraphReconstruction::ConsolidateDefaults() {
	/* Defaults land on the leaf pin they were read for, but only what a pin stores on itself is
	 * saved: on load the editor rebuilds a pin's children from the parent's own string, and a
	 * parent that was never written empties every leaf under it. GetDefaultValue composes the
	 * whole value from the leaves, so writing that back onto the root is what makes it real. */
	for (const FNode& Node : Nodes) {
		if (Node.Node == nullptr) continue;

		for (URigVMPin* Pin : Node.Node->GetPins()) {
			if (Pin->GetDirection() == ERigVMPinDirection::Output || Pin->IsExecuteContext()) continue;
			if (Pin->GetSubPins().Num() == 0) continue;

			const FString Composed = Pin->GetDefaultValue();

			if (Composed.IsEmpty() || Composed == TEXT("()")) continue;

			RigSetPinDefault(GetController(Node.Node), Pin->GetPinPath(), Composed);
		}
	}
}

void FRigVMGraphReconstruction::ApplyDefaults() {
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		/* A unit, or a call whose arguments are literals the same way a unit's pins are */
		if (Node.Node == nullptr || (Node.Function == nullptr && Node.Invokes == INDEX_NONE)) continue;

		for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
			if (!Node.Arguments.IsValidIndex(OperandIndex) || Node.Arguments[OperandIndex].IsEmpty()) continue;

			const int32 MemoryType = Node.Operands[OperandIndex].Key;

			/* A literal is the value the pin was authored with. A work register the node owns is
			 * one of its own inputs, and carries whatever the pin was left at. */
			if (MemoryType != MemoryTypeLiteral && MemoryType != MemoryTypeWork) continue;

			const FRegister* Register = FindRegister(MemoryType, Node.Operands[OperandIndex].Value);

			if (Register == nullptr || !Register->Value.IsValid()) continue;
			if (MemoryType == MemoryTypeWork && Register->Owner != NodeIndex) continue;

			URigVMPin* Pin = Node.Node->FindPin(Node.Arguments[OperandIndex]);

			if (Pin == nullptr || Pin->GetDirection() == ERigVMPinDirection::Output) continue;

			if (SetPinDefault(Pin, Register->Value)) {
				Stats.Defaults++;
			}
		}
	}
}

URigVMPin* FRigVMGraphReconstruction::FindExecuteOutput(URigVMNode* Node, const FString& Label) {
	if (Node == nullptr) {
		return nullptr;
	}

	if (!Label.IsEmpty()) {
		if (URigVMPin* Pin = Node->FindPin(Label)) {
			if (Pin->IsExecuteContext()) {
				return Pin;
			}
		}
	}

	/* A node that runs a block of its own carries on through Completed rather than through the pin
	 * execution came in on */
	if (URigVMPin* Completed = Node->FindPin(TEXT("Completed"))) {
		if (Completed->IsExecuteContext()) {
			return Completed;
		}
	}

	URigVMPin* Fallback = nullptr;

	for (URigVMPin* Pin : Node->GetPins()) {
		if (!Pin->IsExecuteContext()) continue;

		if (Pin->GetDirection() == ERigVMPinDirection::Output) {
			return Pin;
		}

		if (Pin->GetDirection() == ERigVMPinDirection::IO && Fallback == nullptr) {
			Fallback = Pin;
		}
	}

	return Fallback;
}

URigVMPin* FRigVMGraphReconstruction::FindExecuteInput(URigVMNode* Node) {
	if (Node == nullptr) {
		return nullptr;
	}

	for (URigVMPin* Pin : Node->GetPins()) {
		if (!Pin->IsExecuteContext()) continue;

		if (Pin->GetDirection() == ERigVMPinDirection::Input || Pin->GetDirection() == ERigVMPinDirection::IO) {
			return Pin;
		}
	}

	return nullptr;
}

bool FRigVMGraphReconstruction::Link(const int32 SourceNode, const FString& SourcePin, const int32 TargetNode, const FString& TargetPin) {
	if (!Nodes.IsValidIndex(SourceNode) || !Nodes.IsValidIndex(TargetNode)) {
		return false;
	}

	URigVMNode* Source = Nodes[SourceNode].Node;
	URigVMNode* Target = Nodes[TargetNode].Node;

	if (Source == nullptr || Target == nullptr || Source == Target || Source->GetGraph() != Target->GetGraph()) {
		return false;
	}

	URigVMController* LinkController = GetController(Source);

	URigVMPin* OutputPin = Source->FindPin(SourcePin);
	URigVMPin* InputPin = Target->FindPin(TargetPin);

	if (OutputPin == nullptr || InputPin == nullptr) {
		return false;
	}

	/* Already linked, which happens when the same value reaches a pin twice: once as the register
	 * the instruction reads and once as the copy the compiler wrote to fill it */
	for (const URigVMLink* ExistingLink : InputPin->GetSourceLinks()) {
		if (ExistingLink->GetSourcePin() == OutputPin) {
			return false;
		}
	}

	if (LinkController->AddLink(OutputPin->GetPinPath(), InputPin->GetPinPath(), false, false)) {
		Stats.Links++;

		return true;
	}

	/* Both ends can name a pin the same way, and a register only says which pins share a value
	 * rather than which way it flows. Refused one way round, the graph is asked the other. */
	if (LinkController->AddLink(InputPin->GetPinPath(), OutputPin->GetPinPath(), false, false)) {
		Stats.Links++;

		return true;
	}

	/* A dispatch was resolved to the types the bytecode's function name declared, and that name is
	 * not always the truth: an export can carry one entry for every instantiation of the dispatch,
	 * bool arrays standing in for transform ones. The registers know better, so a template whose
	 * types refuse the link goes back to wildcards and takes its types from the link instead. */
	auto RetryUnresolved = [this, LinkController, OutputPin, InputPin](URigVMNode* TemplateNode, const URigVMPin* TemplatePin, const URigVMPin* OtherPin) -> bool {
		if (Cast<URigVMTemplateNode>(TemplateNode) == nullptr) return false;
		if (TemplatePin->GetCPPType() == OtherPin->GetCPPType()) return false;

		if (!LinkController->UnresolveTemplateNodes(TArray<URigVMNode*>{ TemplateNode }, false)) return false;

		if (LinkController->AddLink(OutputPin->GetPinPath(), InputPin->GetPinPath(), false, false)) {
			Stats.Links++;

			return true;
		}

		return false;
	};

	if (RetryUnresolved(InputPin->GetNode(), InputPin, OutputPin)) return true;
	if (RetryUnresolved(OutputPin->GetNode(), OutputPin, InputPin)) return true;

	UE_LOG(LogReflection, Verbose, TEXT("\"%s\" wouldn't link to \"%s\""), *OutputPin->GetPinPath(), *InputPin->GetPinPath());

	Stats.FailedLinks++;

	return false;
}

FString FRigVMGraphReconstruction::GetExternalVariable(const int32 RegisterIndex) const {
	const TArray<FRigVMGraphVariableDescription> Variables = Blueprint->GetMemberVariables();

	if (!Variables.IsValidIndex(RegisterIndex)) {
		return FString();
	}

	return Variables[RegisterIndex].Name.ToString();
}

URigVMNode* FRigVMGraphReconstruction::FindOrAddVariableGetter(const FString& VariableName, const int32 ConsumerNode) {
	if (VariableName.IsEmpty()) {
		return nullptr;
	}

	/* One getter per node reading the variable rather than one shared by the whole graph: a shared
	 * getter feeding readers all over the page is that many links crossing everything between
	 * them, and a Get node costs nothing to repeat. Keyed per consumer so a node reading the same
	 * variable on two pins still only gets one. */
	const FString CacheKey = FString::Printf(TEXT("%s|%d"), *VariableName, ConsumerNode);

	if (URigVMNode** Existing = VariableGetters.Find(CacheKey)) {
		return *Existing;
	}

	const TArray<FRigVMGraphVariableDescription> Variables = Blueprint->GetMemberVariables();
	const FRigVMGraphVariableDescription* Variable = Variables.FindByPredicate([&VariableName](const FRigVMGraphVariableDescription& Description) {
		return Description.Name.ToString() == VariableName;
	});

	if (Variable == nullptr) {
		return nullptr;
	}

	URigVMNode* Getter = GetController(Nodes[ConsumerNode].Callable)->AddVariableNode(
		Variable->Name,
		Variable->CPPType,
		Variable->CPPTypeObject,
		true,
		FString(),
		FVector2D(-400.0f, 260.0f * static_cast<float>(VariableGetters.Num())),
		FString(),
		false,
		false
	);

	if (Getter != nullptr) {
		Stats.VariableNodes++;

		VariableGetters.Add(CacheKey, Getter);
	}

	return Getter;
}

void FRigVMGraphReconstruction::CreateVariableSetters(const TArray<FUObjectJsonValueExport>& Instructions) {
	/* A copy between two work registers says they hold the same value. When one side belongs to a
	 * pin and the other belongs to nothing, a local variable of a collapsed graph being what does
	 * that, the unowned side takes the owned side's pin: every reader then links to the value's
	 * real producer. Repeated until nothing changes, since copies chain through one another. */
	for (bool bChanged = true; bChanged;) {
		bChanged = false;

		for (const FUObjectJsonValueExport& Instruction : Instructions) {
			if (Instruction.GetInteger(TEXT("OpCode"), -1) != OpCodeCopy) continue;

			const FUObjectJsonValueExport Source = Instruction.GetObject(TEXT("Source"));
			const FUObjectJsonValueExport Target = Instruction.GetObject(TEXT("Target"));

			if (!Source.JsonObject.IsValid() || !Target.JsonObject.IsValid()) continue;

			/* Only whole values alias: a copy into part of a register is a link, made below */
			if (Source.GetInteger(TEXT("RegisterOffset"), NoRegisterOffset) != NoRegisterOffset) continue;
			if (Target.GetInteger(TEXT("RegisterOffset"), NoRegisterOffset) != NoRegisterOffset) continue;

			if (Source.GetInteger(TEXT("MemoryType"), INDEX_NONE) != MemoryTypeWork) continue;
			if (Target.GetInteger(TEXT("MemoryType"), INDEX_NONE) != MemoryTypeWork) continue;

			FRegister* SourceRegister = FindRegister(MemoryTypeWork, Source.GetInteger(TEXT("RegisterIndex"), INDEX_NONE));
			FRegister* TargetRegister = FindRegister(MemoryTypeWork, Target.GetInteger(TEXT("RegisterIndex"), INDEX_NONE));

			if (SourceRegister == nullptr || TargetRegister == nullptr) continue;

			if (SourceRegister->Owner != INDEX_NONE && TargetRegister->Owner == INDEX_NONE) {
				TargetRegister->Owner = SourceRegister->Owner;
				TargetRegister->OwnerPin = SourceRegister->OwnerPin;

				bChanged = true;
			} else if (TargetRegister->Owner != INDEX_NONE && SourceRegister->Owner == INDEX_NONE) {
				SourceRegister->Owner = TargetRegister->Owner;
				SourceRegister->OwnerPin = TargetRegister->OwnerPin;

				bChanged = true;
			}
		}
	}

	for (int32 Index = 0; Index < Instructions.Num(); ++Index) {
		const FUObjectJsonValueExport& Instruction = Instructions[Index];

		if (Instruction.GetInteger(TEXT("OpCode"), -1) != OpCodeCopy) continue;

		const FUObjectJsonValueExport Source = Instruction.GetObject(TEXT("Source"));
		const FUObjectJsonValueExport Target = Instruction.GetObject(TEXT("Target"));

		if (!Source.JsonObject.IsValid() || !Target.JsonObject.IsValid()) continue;
		if (Target.GetInteger(TEXT("MemoryType"), INDEX_NONE) != MemoryTypeExternal) continue;

		const int32 TargetIndex = Target.GetInteger(TEXT("RegisterIndex"), INDEX_NONE);
		const FRegister* SourceRegister = FindRegister(Source.GetInteger(TEXT("MemoryType"), INDEX_NONE), Source.GetInteger(TEXT("RegisterIndex"), INDEX_NONE));

		/* Written into one of the rig's variables: a set node, which the execution picks up along
		 * with everything else at this instruction, and which the value is linked into later */
		if (SourceRegister == nullptr || SourceRegister->Owner == INDEX_NONE) continue;

		const FString VariableName = GetExternalVariable(TargetIndex);
		const TArray<FRigVMGraphVariableDescription> Variables = Blueprint->GetMemberVariables();

		const FRigVMGraphVariableDescription* Variable = Variables.FindByPredicate([&VariableName](const FRigVMGraphVariableDescription& Description) {
			return Description.Name.ToString() == VariableName;
		});

		if (Variable == nullptr) continue;

		URigVMNode* Setter = GetController(FindCallable(Index))->AddVariableNode(
			Variable->Name,
			Variable->CPPType,
			Variable->CPPTypeObject,
			false,
			FString(),
			FVector2D(400.0f * static_cast<float>(Index / 12), 260.0f * static_cast<float>(Index % 12)),
			FString(),
			false,
			false
		);

		if (Setter == nullptr) continue;

		Stats.VariableNodes++;

		FNode SetterNode;
		SetterNode.Instruction = Index;
		SetterNode.Callable = FindCallable(Index);
		SetterNode.Name = Setter->GetName();
		SetterNode.Node = Setter;

		InstructionToNode.Add(Index, Nodes.Add(SetterNode));
	}
}

void FRigVMGraphReconstruction::CreateLinks(const TArray<FUObjectJsonValueExport>& Instructions) {
	/* Data links: an operand naming a register some other node's pin owns is that pin reaching
	 * this one */
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		if (Node.Node == nullptr) continue;

		for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
			if (!Node.Arguments.IsValidIndex(OperandIndex)) continue;

			const int32 MemoryType = Node.Operands[OperandIndex].Key;
			const int32 RegisterIndex = Node.Operands[OperandIndex].Value;

			const FString PinName = Node.Arguments[OperandIndex];

			/* One of the rig's own variables, read straight into a pin */
			if (MemoryType == MemoryTypeExternal) {
				URigVMNode* Getter = FindOrAddVariableGetter(GetExternalVariable(RegisterIndex), NodeIndex);

				if (Getter == nullptr) continue;

				URigVMPin* ValuePin = Getter->FindPin(TEXT("Value"));
				URigVMPin* TargetPin = Node.Node->FindPin(PinName);

				if (ValuePin != nullptr && TargetPin != nullptr) {
					if (GetController(Getter)->AddLink(ValuePin->GetPinPath(), TargetPin->GetPinPath(), false, false)) {
						Stats.Links++;
					} else {
						Stats.FailedLinks++;
					}
				}

				continue;
			}

			/* Work registers are wired below, in the order the VM touches them */
		}
	}

	/* One register, several nodes: the value flows through them in the order the VM runs, each
	 * writer handing to whatever touches the register next. Wiring everything to a single owner
	 * instead loses that order: an array a SetNum sizes, a SetAtIndex fills and a reader consumes
	 * would show all three hanging off the first node. */
	struct FToucher {
		int32 Instruction = INDEX_NONE;
		int32 Node = INDEX_NONE;

		FString Pin;
	};

	TMap<int32, TArray<FToucher>> Touchers;

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) {
		FNode& Node = Nodes[NodeIndex];

		if (Node.Node == nullptr) continue;

		for (int32 OperandIndex = 0; OperandIndex < Node.Operands.Num(); ++OperandIndex) {
			if (!Node.Arguments.IsValidIndex(OperandIndex)) continue;
			if (Node.Arguments[OperandIndex].IsEmpty()) continue;
			if (Node.Operands[OperandIndex].Key != MemoryTypeWork) continue;

			Touchers.FindOrAdd(Node.Operands[OperandIndex].Value).Add({ Node.Instruction, NodeIndex, Node.Arguments[OperandIndex] });
		}
	}

	for (TPair<int32, TArray<FToucher>>& Pair : Touchers) {
		Pair.Value.Sort([](const FToucher& A, const FToucher& B) {
			return A.Instruction < B.Instruction;
		});

		const FRegister* Register = FindRegister(MemoryTypeWork, Pair.Key);

		int32 LastWriterNode = INDEX_NONE;
		FString LastWriterPin;

		for (const FToucher& Toucher : Pair.Value) {
			URigVMNode* TouchingNode = Nodes[Toucher.Node].Node;
			if (TouchingNode == nullptr) continue;

			const URigVMPin* Pin = TouchingNode->FindPin(Toucher.Pin);
			if (Pin == nullptr) continue;

			const ERigVMPinDirection Direction = Pin->GetDirection();
			const bool bReads = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO;
			const bool bWrites = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;

			if (bReads) {
				if (LastWriterNode != INDEX_NONE && LastWriterNode != Toucher.Node) {
					Link(LastWriterNode, LastWriterPin, Toucher.Node, Toucher.Pin);
				} else if (LastWriterNode == INDEX_NONE && Register != nullptr && Register->Owner != INDEX_NONE && Register->Owner != Toucher.Node) {
					/* Read before any writer has run, a loop carrying a value back being what does
					 * that: the register's owner is the best that can be said about the source */
					Link(Register->Owner, Register->OwnerPin, Toucher.Node, Toucher.Pin);
				}
			}

			if (bWrites) {
				LastWriterNode = Toucher.Node;
				LastWriterPin = Toucher.Pin;
			}
		}
	}

	/* Copies are what the compiler writes where a register cannot be shared: between two pins of
	 * types that don't match, and in and out of the rig's variables */
	for (int32 Index = 0; Index < Instructions.Num(); ++Index) {
		const FUObjectJsonValueExport& Instruction = Instructions[Index];

		if (Instruction.GetInteger(TEXT("OpCode"), -1) != OpCodeCopy) continue;

		const FUObjectJsonValueExport Source = Instruction.GetObject(TEXT("Source"));
		const FUObjectJsonValueExport Target = Instruction.GetObject(TEXT("Target"));

		if (!Source.JsonObject.IsValid() || !Target.JsonObject.IsValid()) continue;

		const int32 SourceMemory = Source.GetInteger(TEXT("MemoryType"), INDEX_NONE);
		const int32 TargetMemory = Target.GetInteger(TEXT("MemoryType"), INDEX_NONE);

		const int32 SourceIndex = Source.GetInteger(TEXT("RegisterIndex"), INDEX_NONE);
		const int32 TargetIndex = Target.GetInteger(TEXT("RegisterIndex"), INDEX_NONE);

		const FRegister* SourceRegister = FindRegister(SourceMemory, SourceIndex);
		const FRegister* TargetRegister = FindRegister(TargetMemory, TargetIndex);

		/* A copy in or out of part of a register is one field of a struct being filled in rather
		 * than the whole of it: the pin it reaches is the sub pin that field belongs to, which the
		 * memory's own list of property paths names. */
		const FString SourceSegment = GetSegmentPath(SourceMemory, Source.GetInteger(TEXT("RegisterOffset"), NoRegisterOffset));
		const FString TargetSegment = GetSegmentPath(TargetMemory, Target.GetInteger(TEXT("RegisterOffset"), NoRegisterOffset));

		auto WithSegment = [](const FString& PinName, const FString& Segment) {
			return Segment.IsEmpty() ? PinName : PinName + TEXT(".") + Segment;
		};

		/* Written into one of the rig's variables: the set node made for this instruction */
		if (TargetMemory == MemoryTypeExternal) {
			const int32* SetterIndex = InstructionToNode.Find(Index);

			if (SetterIndex != nullptr && SourceRegister != nullptr && SourceRegister->Owner != INDEX_NONE) {
				Link(SourceRegister->Owner, WithSegment(SourceRegister->OwnerPin, SourceSegment), *SetterIndex, TEXT("Value"));
			}

			continue;
		}

		/* Read out of one of the rig's variables into a pin */
		if (SourceMemory == MemoryTypeExternal) {
			if (TargetRegister == nullptr || TargetRegister->Owner == INDEX_NONE) continue;

			URigVMNode* Getter = FindOrAddVariableGetter(GetExternalVariable(SourceIndex), TargetRegister->Owner);

			if (Getter == nullptr) continue;

			URigVMPin* ValuePin = Getter->FindPin(TEXT("Value"));
			URigVMPin* TargetPin = Nodes[TargetRegister->Owner].Node != nullptr
				? Nodes[TargetRegister->Owner].Node->FindPin(WithSegment(TargetRegister->OwnerPin, TargetSegment))
				: nullptr;

			if (ValuePin != nullptr && TargetPin != nullptr) {
				if (GetController(Getter)->AddLink(ValuePin->GetPinPath(), TargetPin->GetPinPath(), false, false)) {
					Stats.Links++;
				} else {
					Stats.FailedLinks++;
				}
			}

			continue;
		}

		/* A literal copied into a pin is that pin's own value rather than anything linked. A copy
		 * out of part of the literal carries one field, the source segment naming which: a rig's
		 * own struct copied member by member into a node's pins is what these look like, and the
		 * segment is what maps the struct's field names onto the pin's. */
		if (SourceMemory == MemoryTypeLiteral) {
			if (TargetRegister == nullptr || TargetRegister->Owner == INDEX_NONE) continue;
			if (SourceRegister == nullptr || !SourceRegister->Value.IsValid()) continue;

			URigVMNode* TargetNode = Nodes[TargetRegister->Owner].Node;

			if (TargetNode == nullptr) continue;

			TSharedPtr<FJsonValue> SourceValue = SourceRegister->Value;

			if (!SourceSegment.IsEmpty()) {
				SourceValue = ResolveJsonSegment(SourceValue, SourceSegment);
			}

			if (!SourceValue.IsValid()) continue;

			if (SetPinDefault(TargetNode->FindPin(WithSegment(TargetRegister->OwnerPin, TargetSegment)), SourceValue)) {
				Stats.Defaults++;
			}

			continue;
		}

		if (SourceRegister == nullptr || TargetRegister == nullptr) continue;
		if (SourceRegister->Owner == INDEX_NONE || TargetRegister->Owner == INDEX_NONE) continue;

		Link(
			SourceRegister->Owner, WithSegment(SourceRegister->OwnerPin, SourceSegment),
			TargetRegister->Owner, WithSegment(TargetRegister->OwnerPin, TargetSegment)
		);
	}
}

void FRigVMGraphReconstruction::CreateExecution(const TArray<FUObjectJsonValueExport>& Instructions, const FUObjectJsonValueExport& ByteCode) {
	TArray<FBlock> Blocks;

	/* The branch table says which instructions run from which pin of a control flow node. The
	 * instruction it names is the jump the compiler wrote, and the node it belongs to is the one
	 * that ran just before it. */
	if (ByteCode.Has(TEXT("BranchInfos"))) {
		for (const FUObjectJsonValueExport& BranchInfo : ByteCode.GetArray(TEXT("BranchInfos"))) {
			FBlock Block;
			Block.First = BranchInfo.GetInteger(TEXT("FirstInstruction"), INDEX_NONE);
			Block.Last = BranchInfo.GetInteger(TEXT("LastInstruction"), INDEX_NONE);
			Block.Label = BranchInfo.Has(TEXT("Label")) ? BranchInfo.GetString(TEXT("Label")) : FString();

			/* An argument index means the block is a value the node asks for when it needs it,
			 * rather than a run of execution hanging off one of its pins */
			Block.bLazy = BranchInfo.GetInteger(TEXT("ArgumentIndex"), INDEX_NONE) >= 0;

			const int32 Instruction = BranchInfo.GetInteger(TEXT("InstructionIndex"), INDEX_NONE);

			for (int32 Index = Instruction; Index >= 0; --Index) {
				if (InstructionToNode.Contains(Index)) {
					Block.Owner = Index;

					break;
				}
			}

			if (Block.First <= Block.Last && Block.First != INDEX_NONE) {
				Blocks.Add(Block);
			}
		}
	}

	/* Everything a lazily run block holds is a value computed on demand, so it is left out of the
	 * execution rather than chained into whatever ran around it */
	for (const FUObjectJsonValueExport& Instruction : Instructions) {
		if (Instruction.GetInteger(TEXT("OpCode"), -1) != OpCodeRunInstructions) continue;

		FBlock Block;
		Block.First = Instruction.GetInteger(TEXT("StartInstruction"), INDEX_NONE);
		Block.Last = Instruction.GetInteger(TEXT("EndInstruction"), INDEX_NONE);
		Block.bLazy = true;

		if (Block.First <= Block.Last && Block.First != INDEX_NONE) {
			Blocks.Add(Block);
		}
	}

	/* The innermost block an instruction sits in is the one that runs it */
	auto FindBlock = [&Blocks](const int32 Instruction) -> const FBlock* {
		const FBlock* Innermost = nullptr;

		for (const FBlock& Block : Blocks) {
			if (Instruction < Block.First || Instruction > Block.Last) continue;

			if (Innermost == nullptr || (Block.Last - Block.First) < (Innermost->Last - Innermost->First)) {
				Innermost = &Block;
			}
		}

		return Innermost;
	};

	/* Chained per block, in the order the VM runs them: the first node of a block hangs off the pin
	 * the block belongs to, and everything after it off the node before it */
	TMap<const FBlock*, int32> Previous;

	/* The rig's own graph and every function graph run a chain of their own, keyed by the callable
	 * they belong to. A function's starts at its entry node. */
	TMap<int32, int32> RootPrevious;

	for (const FCallable& Callable : Callables) {
		if (Callable.EntryNode != INDEX_NONE) {
			RootPrevious.Add(Nodes[Callable.EntryNode].Callable, Callable.EntryNode);
		}
	}

	TArray<int32> SortedInstructions;
	InstructionToNode.GetKeys(SortedInstructions);
	SortedInstructions.Sort();

	/* Entries are separate runs of the same graph, so each starts a chain of its own */
	TArray<int32> EntryInstructions;

	if (ByteCode.Has(TEXT("Entries"))) {
		for (const TSharedPtr<FJsonValue>& Entry : ByteCode.JsonObject->GetArrayField(TEXT("Entries"))) {
			EntryInstructions.Add(ReadEntryInstruction(Entry->AsString()));
		}
	}

	for (const int32 Instruction : SortedInstructions) {
		const int32 NodeIndex = InstructionToNode.FindChecked(Instruction);

		URigVMNode* Node = Nodes[NodeIndex].Node;

		if (Node == nullptr) continue;

		/* An event is only ever a source: execution starts at it rather than reaching it. A node
		 * with neither end is a value the graph reads on its way past, and is left out. */
		const bool bTakesExecution = FindExecuteInput(Node) != nullptr;
		const bool bPassesExecution = FindExecuteOutput(Node, FString()) != nullptr;

		if (!bTakesExecution && !bPassesExecution) continue;

		const FBlock* Block = FindBlock(Instruction);

		if (Block != nullptr && Block->bLazy) continue;

		int32 PreviousNode = INDEX_NONE;

		const int32 Chain = Nodes[NodeIndex].Callable;

		if (Block != nullptr) {
			if (const int32* Found = Previous.Find(Block)) {
				PreviousNode = *Found;
			}
		} else {
			/* An entry starts its own chain rather than carrying on from the run before it */
			if (Chain == INDEX_NONE && EntryInstructions.Contains(Instruction)) {
				RootPrevious.Remove(Chain);
			} else if (const int32* Found = RootPrevious.Find(Chain)) {
				PreviousNode = *Found;
			}
		}

		if (!bTakesExecution) {
			/* Nothing to link into, but execution carries on out of it */
		} else if (PreviousNode == INDEX_NONE && Block != nullptr && Block->Owner != INDEX_NONE) {
			/* The first node of a block runs from the pin the block was compiled from */
			if (const int32* OwnerNode = InstructionToNode.Find(Block->Owner)) {
				URigVMPin* OutputPin = FindExecuteOutput(Nodes[*OwnerNode].Node, Block->Label);
				URigVMPin* InputPin = FindExecuteInput(Node);

				if (OutputPin != nullptr && InputPin != nullptr) {
					if (GetController(Node)->AddLink(OutputPin->GetPinPath(), InputPin->GetPinPath(), false, false)) {
						Stats.Links++;
					} else {
						Stats.FailedLinks++;
					}
				}
			}
		} else if (PreviousNode != INDEX_NONE) {
			URigVMPin* OutputPin = FindExecuteOutput(Nodes[PreviousNode].Node, FString());
			URigVMPin* InputPin = FindExecuteInput(Node);

			if (OutputPin != nullptr && InputPin != nullptr) {
				if (GetController(Node)->AddLink(OutputPin->GetPinPath(), InputPin->GetPinPath(), false, false)) {
					Stats.Links++;
				} else {
					Stats.FailedLinks++;
				}
			}
		}

		if (!bPassesExecution) continue;

		if (Block != nullptr) {
			Previous.Add(Block, NodeIndex);
		} else {
			RootPrevious.Add(Chain, NodeIndex);
		}
	}

	/* A function's chain ends on its return node */
	for (const FCallable& Callable : Callables) {
		if (Callable.EntryNode == INDEX_NONE || Callable.ReturnNode == INDEX_NONE) continue;

		const int32* Last = RootPrevious.Find(Nodes[Callable.EntryNode].Callable);

		if (Last == nullptr || *Last == Callable.EntryNode) continue;

		URigVMPin* OutputPin = FindExecuteOutput(Nodes[*Last].Node, FString());
		URigVMPin* InputPin = FindExecuteInput(Nodes[Callable.ReturnNode].Node);

		if (OutputPin != nullptr && InputPin != nullptr) {
			if (GetController(InputPin->GetNode())->AddLink(OutputPin->GetPinPath(), InputPin->GetPinPath(), false, false)) {
				Stats.Links++;
			} else {
				Stats.FailedLinks++;
			}
		}
	}
}

#endif
