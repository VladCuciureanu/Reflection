/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A Niagara system, with the emitters it carries inside its own package.
 *
 * A system is two scripts of its own (system spawn and update, written in one graph), a handle per
 * emitter, and the emitters themselves, which sit under the system as exports rather than in
 * packages of their own. The properties bring the handles and the scripts across; what the cook
 * left out is the wiring of the graphs, the scripts' sources and each emitter's version list, and
 * those are put back the way the script and emitter importers put them back. The system is then
 * compiled from its graphs, since the compiled data the cook kept is for the game, not the editor. */
class INiagaraSystemImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraSystemImporter, TArray<FString>{
	TEXT("NiagaraSystem")
}, TEXT("Niagara Assets"));
