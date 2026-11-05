/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraSystemImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"
#include "Importers/Types/Niagara/NiagaraEmitterRepair.h"

#include "Engine/EngineUtilities.h"

#include "NiagaraSystem.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSystemImpl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraEmitter.h"
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

bool INiagaraSystemImporter::Import() {
	/* The graphs, and the script sources that hold them, come from the package's editor copy. Without one there is
	 * nothing to give the system's scripts as their source, and an editor project has no use for a system without:
	 * bringing it up (UpdateSystemAfterLoad, which saving runs through PreSave) ensures on the missing source and
	 * then reads through it, taking the editor down. FN_ImpactMajor_Burst_Baseline, the performance baseline an
	 * effect type names, came that way on Fortnite 43.00. Such a system is left out and said so, and a reference to
	 * it stays empty. */
	if (FUObjectExportContainer* Container = GetContainer()) {
		const bool bHasSource = Container->Exports.ContainsByPredicate([](FUObjectExport* Export) {
			return Export != nullptr && Export->IsJsonValid() && Export->GetType() == TEXT("NiagaraScriptSource");
		});

		if (!bHasSource) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("No graph to build from"),
				FString::Printf(TEXT("'%s' came without its editor copy, so it has no graphs for its scripts; the editor can't load a Niagara system like that, so it was left out and references to it are empty."), *GetAssetName())
			);

			UE_LOG(LogReflection, Warning, TEXT("\"%s\" carries no graph source (no editor copy came across); not built"), *GetAssetName());

			return false;
		}
	}

	return IObjectImporter::Import();
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

	/* And every emitter's stages, the parents' copies under them included, which nothing else repairs */
	int32 Unstaged = 0;

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->Object != nullptr && Export->GetType() == TEXT("NiagaraEmitter")) Unstaged += FNiagaraEmitterRepair::DropScriptlessStages(Export->Object);
	}

	if (Unstaged > 0) {
		UE_LOG(LogReflection, Display, TEXT("\"%s\" left out %d simulation stage(s) with no script"), *GetAssetName(), Unstaged);
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

	/* Every emitter in the package, the handles' and the ones nested inside them (NS_Emote_StarStray's
	 * FN_DebugGnomon carries another FN_DebugGnomon), gets the scripts its PostLoad walks
	 * (FNiagaraEmitterRepair::GiveRequiredScripts). The repair above only reaches the emitters directly under the
	 * system, whose scripts are in place by now. */
	int32 ScriptsMade = 0;

	/* A renderer the cook left out (FN_DebugGnomon's, debug only) is an empty entry, which a cooked package may have
	 * and the editor's PostLoad takes out with an ensure for any other: it goes here instead. */
	int32 RenderersDropped = 0;

	if (FUObjectExportContainer* EmitterContainer = GetContainer()) {
		for (FUObjectExport* Export : EmitterContainer->Exports) {
			UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Export->Object);
			if (Emitter == nullptr) continue;

			for (const FNiagaraVersion& Version : FNiagaraVersions::All(Emitter)) {
				RenderersDropped += FNiagaraVersions::RemoveNulls(Version, TEXT("RendererProperties"));

				ScriptsMade += FNiagaraEmitterRepair::GiveRequiredScripts(Emitter, Version, FNiagaraVersions::GetObject(Version, TEXT("GraphSource")));

				/* And the GPU compute script the editor gives every emitter version (FN_ImpactMajor_Burst_Baseline's
				 * Fireworks_Mesh_1 names one the cook left out): a GPU emitter's compile walks it, and took the editor
				 * down on a null one. */
				if (FNiagaraVersions::GetObject(Version, TEXT("GPUComputeScript")) == nullptr) {
					UNiagaraScript* Script = NewObject<UNiagaraScript>(Emitter, NAME_None, RF_Transactional);
					Script->SetUsage(ENiagaraScriptUsage::ParticleGPUComputeScript);
					if (UNiagaraScriptSourceBase* Source = Cast<UNiagaraScriptSourceBase>(FNiagaraVersions::GetObject(Version, TEXT("GraphSource")))) {
						Script->SetLatestSource(Source);
					}

					if (FNiagaraVersions::SetObject(Version, TEXT("GPUComputeScript"), Script)) ScriptsMade++;
				}
			}
		}
	}

	/* A ribbon's UV settings that came across out of range (NS_Emote_TotalFlock_Slash_Kick_Ribbon's read back as distribution
	 * mode 209 and edge mode MAX) stop the renderer on an assertion the moment the ribbon draws ("Unsupported ribbon uv edge
	 * mode"). The values aren't the game's, so the settings go back to their defaults. */
	int32 RibbonUVsReset = 0;

	if (FUObjectExportContainer* RibbonContainer = GetContainer()) {
		for (const FUObjectExport* Export : RibbonContainer->Exports) {
			UNiagaraRibbonRendererProperties* Ribbon = Cast<UNiagaraRibbonRendererProperties>(Export->Object);
			if (Ribbon == nullptr) continue;

			const UNiagaraRibbonRendererProperties* Defaults = GetDefault<UNiagaraRibbonRendererProperties>();

			for (const TPair<FNiagaraRibbonUVSettings*, const FNiagaraRibbonUVSettings*> Pair : {
				TPair<FNiagaraRibbonUVSettings*, const FNiagaraRibbonUVSettings*>(&Ribbon->UV0Settings, &Defaults->UV0Settings),
				TPair<FNiagaraRibbonUVSettings*, const FNiagaraRibbonUVSettings*>(&Ribbon->UV1Settings, &Defaults->UV1Settings) }) {
				FNiagaraRibbonUVSettings* Settings = Pair.Key;
				const bool bValid = static_cast<uint8>(Settings->DistributionMode) <= static_cast<uint8>(ENiagaraRibbonUVDistributionMode::TiledFromStartOverRibbonLength)
					&& static_cast<uint8>(Settings->LeadingEdgeMode) <= static_cast<uint8>(ENiagaraRibbonUVEdgeMode::Locked)
					&& static_cast<uint8>(Settings->TrailingEdgeMode) <= static_cast<uint8>(ENiagaraRibbonUVEdgeMode::Locked);

				if (bValid) continue;

				*Settings = *Pair.Value;
				RibbonUVsReset++;
			}
		}
	}

	if (RibbonUVsReset > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d ribbon UV setting(s) came across out of range"), RibbonUVsReset),
			FString::Printf(TEXT("'%s' has ribbon renderers whose UV distribution or edge modes aren't values the engine has; they were reset to the defaults."), *GetAssetName())
		);
	}

	if (RenderersDropped > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d emitter renderer(s) didn't come across"), RenderersDropped),
			FString::Printf(TEXT("'%s' has emitters with renderers the cook left out; they were taken out of the list."), *GetAssetName())
		);
	}

	if (ScriptsMade > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d emitter script(s) were missing"), ScriptsMade),
			FString::Printf(TEXT("'%s' has emitters without a spawn or update script the editor needs; each got an empty one compiled from its graph."), *GetAssetName())
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
	 * compiling the first time something spawns it. A loaded system has been through
	 * UpdateSystemAfterLoad before anything compiles it; one built here hadn't, so its first save
	 * ran it after the compile, and on 5.8 that walk (into CacheFromCompiledData) read freed memory
	 * and took the editor down. It goes first here, as a load does. */
	System->EnsureFullyLoaded();
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
