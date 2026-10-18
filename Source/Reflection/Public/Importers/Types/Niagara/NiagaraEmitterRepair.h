/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

struct FUObjectExportContainer;
struct FNiagaraGraphStats;

/* An emitter's versions filled from what sits under it.
 *
 * A standalone emitter and an emitter a system carries inside its own package come across the same
 * way, so the one repair serves both: the emitter importer for the first, the system importer once
 * per emitter for the second. The graphs and the cooked cache are put back beforehand, once per
 * package, by whoever calls this. */
class REFLECTION_API FNiagaraEmitterRepair {
public:
	static void Repair(FUObjectExportContainer* Container, UObject* Emitter, const FString& Name,
		const FNiagaraGraphStats& Stats, int32 Restored);

	/* Takes out of each of the emitter's versions the simulation stages that are nothing or have no script, returns
	 * how many. A parent's copy names stages and scripts the cook kept only under the emitter made from it, and
	 * every load of it ensures on such an entry; it plays nothing of them. */
	static int32 DropScriptlessStages(UObject* Emitter);
};
