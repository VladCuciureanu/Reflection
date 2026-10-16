/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraSystemImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"
#include "Importers/Types/Niagara/NiagaraEmitterRepair.h"

#include "Engine/EngineUtilities.h"

#include "NiagaraSystem.h"
#include "NiagaraSystemImpl.h"
#include "NiagaraScript.h"
#include "NiagaraDataInterface.h"

#include "UObject/UnrealType.h"

namespace {
	UObject* ObjectField(UObject* Owner, const TCHAR* Field) {
		const FObjectPropertyBase* Property = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Field) : nullptr;

		return Property ? Property->GetObjectPropertyValue_InContainer(Owner) : nullptr;
	}

	bool SetObjectField(UObject* Owner, const TCHAR* Field, UObject* Value) {
		const FObjectPropertyBase* Property = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Field) : nullptr;

		if (Property == nullptr || (Value != nullptr && !Value->IsA(Property->PropertyClass))) return false;

		Property->SetObjectPropertyValue_InContainer(Owner, Value);

		return true;
	}
}

void INiagaraSystemImporter::Repair(UObject* Asset) const {
	FUObjectExportContainer* Container = GetContainer();

	if (Container == nullptr) return;

	/* The graphs and the cooked cache once for the whole package: the system's graph and every
	 * emitter's are in it */
	FNiagaraGraphStats Stats;

	FNiagaraGraphs::Rebuild(Container, GetPackage(), GetPropertySerializer(), Stats);

	const int32 Restored = FNiagaraCookedCache::Restore(Container);

	if (Stats.Unpaired > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d pin pairing(s) name a pin that isn't there"), Stats.Unpaired),
			FString::Printf(
				TEXT("'%s' has nodes that pair their own pins off and name one that didn't come across, so the node drops the pairing and whatever fed the pin stops reaching what it stood for."),
				*GetAssetName())
		);
	}

	/* What sits directly under the system: its own graph's source, its editor data and its emitters */
	TArray<UObject*> Sources;
	TArray<UObject*> EditorData;
	TArray<UObject*> Emitters;

	for (FUObjectExport* Export : Container->Exports) {
		UObject* Object = Export->Object;

		if (Object == nullptr || Object->GetOuter() != Asset) continue;

		const FString Type = Export->GetType().ToString();

		if (Type == TEXT("NiagaraScriptSource")) Sources.Add(Object);
		else if (Type == TEXT("NiagaraSystemEditorData")) EditorData.Add(Object);
		else if (Type == TEXT("NiagaraEmitter")) Emitters.Add(Object);
	}

	/* The system's two scripts are written in the one graph. A script names its source in its
	 * version list; where that did not come across, the source under the system is the only one
	 * it can be. */
	int32 Sourced = 0;

	for (const TCHAR* Field : { TEXT("SystemSpawnScript"), TEXT("SystemUpdateScript") }) {
		UObject* Script = ObjectField(Asset, Field);

		if (Script == nullptr) continue;

		const FNiagaraVersion Version = FNiagaraVersions::Only(Script);

		if (!Version.IsValid() || FNiagaraVersions::HasObject(Version, TEXT("Source"))) continue;

		if (Sources.Num() > 0 && FNiagaraVersions::SetObject(Version, TEXT("Source"), Sources[0])) Sourced++;
	}

	if (Sources.Num() > 1) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d system graphs to choose between"), Sources.Num()),
			FString::Printf(TEXT("'%s' carries %d sources directly under it; its scripts that named none were given the first."),
				*GetAssetName(), Sources.Num())
		);
	}

	if (ObjectField(Asset, TEXT("EditorData")) == nullptr && EditorData.Num() > 0) {
		SetObjectField(Asset, TEXT("EditorData"), EditorData[0]);
	}

	/* Each emitter as an emitter import would leave it */
	for (UObject* Emitter : Emitters) {
		FNiagaraEmitterRepair::Repair(Container, Emitter, Emitter->GetName(), Stats, Restored);
	}

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" put %d node(s) in their graphs, made %d pin(s) and joined %d of them, gave %d system script(s) their source, and repaired %d emitter(s)"),
		*GetAssetName(), Stats.Placed, Stats.Pins, Stats.Links, Sourced, Emitters.Num());
}

void INiagaraSystemImporter::Validate(UObject* Asset) const {
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);

	if (System == nullptr) return;

	/* A handle switched on with no emitter behind it (one that didn't come across) is switched off before
	 * the compile: the compile reads every enabled handle's emitter (the attribute trim in
	 * ComputeVMCompilationId) and takes the editor down on a null one. */
	int32 Dropped = 0;

	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		const bool bStateless = Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless && Handle.GetStatelessEmitter() != nullptr;

		if (Handle.GetIsEnabled() && Handle.GetInstance().Emitter == nullptr && !bStateless) {
			Handle.SetIsEnabled(false, *System, false);
			Dropped++;
		}
	}

	if (Dropped > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d emitter handle(s) name no emitter and were switched off"), Dropped),
			FString::Printf(TEXT("'%s' has enabled handles whose emitter didn't come across; compiling with them in would crash, so they draw nothing."), *GetAssetName())
		);
	}

	/* A stateless emitter's disabled modules are cooked out as nulls (UNiagaraStatelessEmitter::Serialize trims them),
	 * and a cooked load puts the template's module class back in each slot; an editor load doesn't, and the compile then
	 * reads the null (UNiagaraStatelessEmitter::CacheFromCompiledData -> IsModuleEnabled) and takes the editor down:
	 * NS_Emote_BriskReign_Ice has 21 of 25 such slots. Fortnite's template also has one module more than 5.7's (its
	 * SolveVelocitiesAndForces is the 10th of 25, 5.7's the 9th of 24), so the slots don't line up by index. The modules
	 * are rebuilt in 5.7's template order as UNiagaraStatelessEmitter does when its template changes: each class keeps
	 * the module the emitter has of it, and a class it has none of gets one switched off, as the cook found it. Read and
	 * set through reflection, since the stateless types are internal to Niagara. */
	int32 Restored = 0;

	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		UObject* Stateless = reinterpret_cast<UObject*>(Handle.GetStatelessEmitter());
		if (Stateless == nullptr) continue;

		const FArrayProperty* Modules = FindFProperty<FArrayProperty>(Stateless->GetClass(), TEXT("Modules"));
		const FObjectProperty* TemplateProperty = FindFProperty<FObjectProperty>(Stateless->GetClass(), TEXT("EmitterTemplate"));
		UObject* Template = TemplateProperty ? TemplateProperty->GetObjectPropertyValue_InContainer(Stateless) : nullptr;
		const FArrayProperty* Classes = Template ? FindFProperty<FArrayProperty>(Template->GetClass(), TEXT("Modules")) : nullptr;
		const FObjectPropertyBase* ModuleElement = Modules ? CastField<FObjectPropertyBase>(Modules->Inner) : nullptr;
		const FObjectPropertyBase* ClassElement = Classes ? CastField<FObjectPropertyBase>(Classes->Inner) : nullptr;
		if (ModuleElement == nullptr || ClassElement == nullptr) continue;

		FScriptArrayHelper ModuleArray(Modules, Modules->ContainerPtrToValuePtr<void>(Stateless));
		FScriptArrayHelper ClassArray(Classes, Classes->ContainerPtrToValuePtr<void>(Template));

		TArray<UObject*> Existing;
		for (int32 Index = 0; Index < ModuleArray.Num(); ++Index) {
			if (UObject* Module = ModuleElement->GetObjectPropertyValue(ModuleArray.GetRawPtr(Index))) Existing.Add(Module);
		}

		TArray<UObject*> Rebuilt;
		for (int32 Index = 0; Index < ClassArray.Num(); ++Index) {
			UClass* ModuleClass = Cast<UClass>(ClassElement->GetObjectPropertyValue(ClassArray.GetRawPtr(Index)));
			if (ModuleClass == nullptr) { Rebuilt.Add(nullptr); continue; }

			UObject* const* Kept = Existing.FindByPredicate([ModuleClass](const UObject* Module) { return Module->GetClass() == ModuleClass; });
			if (Kept) { Rebuilt.Add(*Kept); continue; }

			UObject* Module = NewObject<UObject>(Stateless, ModuleClass, NAME_None, RF_Transactional);
			if (const FBoolProperty* Enabled = FindFProperty<FBoolProperty>(ModuleClass, TEXT("bModuleEnabled"))) {
				Enabled->SetPropertyValue_InContainer(Module, false);
			}
			Rebuilt.Add(Module);
			Restored++;
		}

		const bool bChanged = Rebuilt.Num() != ModuleArray.Num() || Rebuilt.ContainsByPredicate([&](const UObject* Module) { return Module == nullptr; })
			|| Existing.Num() != ModuleArray.Num();
		if (!bChanged) continue;

		ModuleArray.Resize(Rebuilt.Num());
		for (int32 Index = 0; Index < Rebuilt.Num(); ++Index) ModuleElement->SetObjectPropertyValue(ModuleArray.GetRawPtr(Index), Rebuilt[Index]);
	}

	if (Restored > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d stateless module(s) were cooked out"), Restored),
			FString::Printf(TEXT("'%s' has stateless emitters whose disabled modules the cook left empty; their modules were rebuilt in the template's order, the missing ones switched off."), *GetAssetName())
		);
	}

	/* A data-interface input node whose interface didn't come across gets a default one of the type it
	 * declares, as the editor gives a new input. The compile collects every such input's interface and
	 * calls into it (the attribute trim), and a null one takes the editor down: FN_ImpactMajor_Burst_Baseline
	 * has them. Read and set through reflection, since the node's accessors aren't exported. */
	int32 Filled = 0;

	if (FUObjectExportContainer* Container = GetContainer()) {
		for (FUObjectExport* Export : Container->Exports) {
			UObject* Node = Export->Object;
			if (Node == nullptr || Node->GetClass()->GetName() != TEXT("NiagaraNodeInput")) continue;

			const FObjectProperty* Held = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("DataInterface"));
			const FStructProperty* Declared = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Input"));
			if (Held == nullptr || Declared == nullptr || Held->GetObjectPropertyValue_InContainer(Node) != nullptr) continue;

			const FNiagaraVariable* Variable = Declared->ContainerPtrToValuePtr<FNiagaraVariable>(Node);
			UClass* Class = Variable->GetType().GetClass();
			if (Class == nullptr || !Class->IsChildOf(UNiagaraDataInterface::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)) continue;

			Held->SetObjectPropertyValue_InContainer(Node, NewObject<UNiagaraDataInterface>(Node, Class, NAME_None, RF_Transactional));
			Filled++;
		}
	}

	if (Filled > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d data interface input(s) came without their interface"), Filled),
			FString::Printf(TEXT("'%s' has data interface inputs whose interface didn't come across; each got a default one of its type, so what it was set up with is lost."), *GetAssetName())
		);
	}

	/* Compiled from the graphs now, so the system is ready to run once it is saved rather than
	 * compiling the first time something spawns it */
	System->RequestCompile(true);
	System->WaitForCompilationComplete(true, false);

	int32 Enabled = 0;
	int32 Missing = 0;

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles()) {
		if (!Handle.GetIsEnabled()) continue;

		Enabled++;

		if (Handle.GetInstance().Emitter == nullptr) Missing++;
	}

	if (Missing > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d of %d emitter handle(s) name no emitter"), Missing, Enabled),
			FString::Printf(TEXT("'%s' has handles whose emitter didn't come across, so they draw nothing."), *GetAssetName())
		);
	}

	/* Asked of the scripts rather than of IsReadyToRun, which also waits on shader and PSO work that
	 * only finishes over the frames to come and so says no straight after any compile */
	int32 Failed = 0;

	System->ForEachScript([&Failed](const UNiagaraScript* Script) {
		if (Script != nullptr && Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error) Failed++;
	});

	if (Failed > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d script(s) did not compile"), Failed),
			FString::Printf(TEXT("'%s' has scripts that failed to compile from its graphs; the Niagara log says which."), *GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" has %d enabled emitter(s)%s and %s"),
		*GetAssetName(), Enabled,
		Missing > 0 ? *FString::Printf(TEXT(", %d without an emitter"), Missing) : TEXT(""),
		Failed > 0 ? *FString::Printf(TEXT("%d script(s) that failed to compile"), Failed) : TEXT("compiled"));
}
