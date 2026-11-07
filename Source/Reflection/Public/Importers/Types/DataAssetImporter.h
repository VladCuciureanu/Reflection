/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IDataAssetImporter : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;
};

/* Style sheets (the game's StyleSheets plugin) are plain objects holding their rules as subobjects; read the same way */
REGISTER_IMPORTER(IDataAssetImporter, (TArray<FString>{
	TEXT("DataAsset"),
	TEXT("StyleSheet"),
	TEXT("StyleSheetInstance")
}), "Data Assets");
