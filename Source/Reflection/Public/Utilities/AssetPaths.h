/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"

/* Turns a package path as Cloud knows it into the one the same asset takes in the editor. */
inline FString ToEditorPackagePath(const FString& InPath) {
	FString Path = InPath;
	/* Absolute package paths are already editor paths. Only a plugin's immediate
	 * Content prefix is an archive alias; a deeper folder named Content is real. */
	if (Path.StartsWith(TEXT("/"))) {
		if (!Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Engine/"))) {
			FString Mount, Rest;
			if (Path.Mid(1).Split(TEXT("/Content/"), &Mount, &Rest) && !Mount.Contains(TEXT("/"))) {
				Path = TEXT("/") + Mount + TEXT("/") + Rest;
			}
		}
		FRRedirects::Redirect(Path);
		return Path;
	}

	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty()) {
		Path = Path.Replace(*(ProjectName + "/Content/"), TEXT("/Game/"));
		Path = Path.Replace(*(ProjectName + "/Plugins"), TEXT(""));
		Path = Path.Replace(TEXT("/Content/"), TEXT("/"));
	}

	Path = Path.Replace(TEXT("Engine/Content"), TEXT("/Engine"));


	if (!Path.StartsWith(TEXT("/"))) {
		Path = "/" + Path;
	}

	FRRedirects::Redirect(Path);

	return Path;
}
