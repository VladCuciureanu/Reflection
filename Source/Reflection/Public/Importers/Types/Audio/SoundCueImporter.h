/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Graph/SoundGraph.h"

class ISoundCueImporter : public ISoundGraph {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

/* Fortnite's cue classes (FortSoundCueTemplate and the emote cues built on it) are SoundCues with a
 * few defaults of their own; their graphs are ordinary cue graphs, so they come across as SoundCues. */
REGISTER_IMPORTER(ISoundCueImporter, (TArray<FString>{
	TEXT("SoundCue"),
	TEXT("FortSoundCueTemplate"),
	TEXT("EmoteFoley"),
	TEXT("EmoteMusic"),
	TEXT("EmoteMusic3P")
}), TEXT("Sound Assets"));