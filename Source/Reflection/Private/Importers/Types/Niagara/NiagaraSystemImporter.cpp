/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraSystemImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"
#include "Importers/Types/Niagara/NiagaraEmitterRepair.h"

#include "Engine/EngineUtilities.h"

#include "NiagaraSystem.h"

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

	if (!System->IsReadyToRun()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The system did not compile"),
			FString::Printf(TEXT("'%s' is not ready to run after compiling its graphs; the Niagara log says which script failed."), *GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" has %d enabled emitter(s)%s and is %s"),
		*GetAssetName(), Enabled,
		Missing > 0 ? *FString::Printf(TEXT(", %d without an emitter"), Missing) : TEXT(""),
		System->IsReadyToRun() ? TEXT("ready to run") : TEXT("not ready to run"));
}
