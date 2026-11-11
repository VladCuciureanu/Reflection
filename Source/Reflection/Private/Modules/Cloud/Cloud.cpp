/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Cloud.h"

#include "Editor.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "TimerManager.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Settings/Runtime.h"
#include "Engine/Log.h"
#include "Engine/EngineUtilities.h"
#include "Modules/Cloud/Remote.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyOptional.h"

/* Requests ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReflectionHttpRequest Cloud::BuildRequest(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	FReflectionHttpRequest NewRequest = FHttpModule::Get().CreateRequest();

	/* A profile named ahead of the path pins the request to it, rather than it going to whichever
	 * one Mirror happens to be showing.
	 *
	 * Not the build endpoints themselves: what Mirror has, and which of them it should open or let
	 * go, are Mirror's own business rather than any one profile's, and pinning those to a profile
	 * that has just been let go would take the question down with it. */
	const bool Pinned = !Build.IsEmpty() && !RequestURL.StartsWith(BuildsURL);

	FString FullUrl = URL + (Pinned ? TEXT("/") + Build : FString()) + RequestURL;

	bool First = true;

	for (const auto& Pair : Parameters) {
		FullUrl += First ? TEXT("?") : TEXT("&");
		First = false;

		FullUrl += FString::Printf(
			TEXT("%s=%s"),
			*FGenericPlatformHttp::UrlEncode(Pair.Key),
			*FGenericPlatformHttp::UrlEncode(Pair.Value)
		);
	}

	for (const auto& Pair : Headers) {
		NewRequest->SetHeader(Pair.Key, Pair.Value);
	}

	NewRequest->SetURL(FullUrl);

	return NewRequest;
}

TSharedPtr<FJsonObject> Cloud::ParseResponse(const FReflectionHttpResponse& Response) {
	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return nullptr;
	}

	if (!Response->GetHeader(TEXT("Content-Type")).Contains(TEXT("json"))) {
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
		return nullptr;
	}

	return JsonObject;
}

void Cloud::Get(const FString& RequestURL,
	const TMap<FString, FString>& Parameters,
	const TMap<FString, FString>& Headers,
	TFunction<void(TSharedPtr<FJsonObject>)> OnComplete)
{
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	FRemoteUtilities::ExecuteRequestAsync(Request, [OnComplete](const FReflectionHttpResponse& Response) {
		OnComplete(ParseResponse(Response));
	});
}

void Cloud::Post(const FString& RequestURL,
	const FString& Body,
	const TMap<FString, FString>& Headers,
	TFunction<void(TSharedPtr<FJsonObject>, int32)> OnComplete)
{
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, {}, Headers);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	FRemoteUtilities::ExecuteRequestAsync(Request, [OnComplete](const FReflectionHttpResponse& Response) {
		/* Zero tells the caller apart from a Cloud that answered but refused */
		const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;

		OnComplete(ParseResponse(Response), ResponseCode);
	});
}

TSharedPtr<FJsonObject> Cloud::PostBlocking(const FString& RequestURL, const FString& Body, const TMap<FString, FString>& Headers, int32* OutResponseCode) {
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, {}, Headers);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

	if (OutResponseCode != nullptr) {
		/* Zero tells a Cloud that never answered apart from one that answered and refused */
		*OutResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;
	}

	return ParseResponse(Response);
}

TSharedPtr<FJsonObject> Cloud::GetBlocking(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	return ParseResponse(FRemoteUtilities::ExecuteRequestBlocking(Request));
}

void Cloud::PumpRequests(const float DeltaSeconds) {
	FHttpModule::Get().GetHttpManager().Tick(DeltaSeconds);
}

void Cloud::RunWhenSafe(TFunction<void()> Work) {
	/* No editor loop to defer onto in the first place */
	if (GEditor == nullptr) {
		Work();

		return;
	}

	/* Timers tick from the editor loop, so next tick is after the HTTP manager tick the response
	 * was delivered from, and after any blocking wait pumping that tick */
	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([Work]() {
		Work();
	}));
}

/* Instanced struct types ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
namespace {
	bool HoldsTypedStructs(const UStruct* Struct, TMap<const UStruct*, bool>& Seen);

	bool HoldsTypedStructs(const FProperty* Property, TMap<const UStruct*, bool>& Seen) {
		if (Property == nullptr) return false;

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property)) {
			if (StructProperty->Struct == FInstancedStruct::StaticStruct() || StructProperty->Struct == FInstancedPropertyBag::StaticStruct()) {
				return true;
			}

			return HoldsTypedStructs(StructProperty->Struct, Seen);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property)) return HoldsTypedStructs(ArrayProperty->Inner, Seen);
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property)) return HoldsTypedStructs(SetProperty->ElementProp, Seen);
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property)) {
			return HoldsTypedStructs(MapProperty->KeyProp, Seen) || HoldsTypedStructs(MapProperty->ValueProp, Seen);
		}
		if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property)) {
			return HoldsTypedStructs(OptionalProperty->GetValueProperty(), Seen);
		}

		/* An instanced subobject is an export of its own, asked about by its own class */
		return false;
	}

	bool HoldsTypedStructs(const UStruct* Struct, TMap<const UStruct*, bool>& Seen) {
		if (Struct == nullptr) return false;
		if (const bool* Known = Seen.Find(Struct)) return *Known;

		/* A struct can't hold itself by value, so one met again while it is being walked is answered no until the
		 * walk that started it finishes */
		Seen.Add(Struct, false);

		bool bHolds = false;

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It) {
			if (HoldsTypedStructs(*It, Seen)) {
				bHolds = true;

				break;
			}
		}

		Seen.Add(Struct, bHolds);

		return bHolds;
	}

	/* What tells one export from another in both lists: the typed export is the raw one's cooked exports in the
	 * same order, but the raw one can be a subset (export_name, export_type) and carries an editor copy's exports
	 * after them. */
	FString ExportKey(const TSharedPtr<FJsonObject>& Export) {
		FString Type, Name, Outer;

		Export->TryGetStringField(TEXT("Type"), Type);
		Export->TryGetStringField(TEXT("Name"), Name);

		if (const TSharedPtr<FJsonObject>* OuterObject = nullptr; Export->TryGetObjectField(TEXT("Outer"), OuterObject)) {
			(*OuterObject)->TryGetStringField(TEXT("ObjectName"), Outer);
		}

		return Type + TEXT("|") + Name + TEXT("|") + Outer;
	}

	/* Copies the typed value's "$struct" names onto the raw value, which is the same value without them. Null when
	 * the raw value was named in place; a value to put in its stead when it has to be replaced: an instanced struct
	 * whose payload isn't an object is written by the typed export as { "$struct", "Value" }. */
	TSharedPtr<FJsonValue> NameStructs(const TSharedPtr<FJsonValue>& Raw, const TSharedPtr<FJsonValue>& Typed, int32& Named) {
		if (!Raw.IsValid() || !Typed.IsValid()) return nullptr;

		if (Typed->Type == EJson::Object) {
			const TSharedPtr<FJsonObject> TypedObject = Typed->AsObject();
			FString StructName;
			const bool bNamed = TypedObject->TryGetStringField(TEXT("$struct"), StructName);

			if (Raw->Type != EJson::Object) {
				if (!bNamed) return nullptr;

				Named++;

				return Typed;
			}

			const TSharedPtr<FJsonObject> RawObject = Raw->AsObject();

			if (bNamed && !RawObject->HasField(TEXT("$struct"))) {
				RawObject->SetStringField(TEXT("$struct"), StructName);
				Named++;
			}

			for (auto& Field : RawObject->Values) {
				if (const TSharedPtr<FJsonValue>* TypedField = TypedObject->Values.Find(Field.Key)) {
					if (TSharedPtr<FJsonValue> Replaced = NameStructs(Field.Value, *TypedField, Named)) {
						Field.Value = Replaced;
					}
				}
			}

			return nullptr;
		}

		if (Typed->Type == EJson::Array && Raw->Type == EJson::Array) {
			const TArray<TSharedPtr<FJsonValue>>& RawArray = Raw->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& TypedArray = Typed->AsArray();

			/* Not the same list: nothing to pair its elements by */
			if (RawArray.Num() != TypedArray.Num()) return nullptr;

			TArray<TSharedPtr<FJsonValue>> Rebuilt;
			bool bReplaced = false;

			for (int32 Index = 0; Index < RawArray.Num(); Index++) {
				TSharedPtr<FJsonValue> Replaced = NameStructs(RawArray[Index], TypedArray[Index], Named);

				if (Replaced.IsValid() && !bReplaced) {
					Rebuilt = RawArray;
					bReplaced = true;
				}

				if (Replaced.IsValid()) Rebuilt[Index] = Replaced;
			}

			if (!bReplaced) return nullptr;

			return MakeShared<FJsonValueArray>(Rebuilt);
		}

		return nullptr;
	}
}

bool Cloud::Export::CanHoldTypedStructs(const UStruct* Struct) {
	TMap<const UStruct*, bool> Seen;

	return HoldsTypedStructs(Struct, Seen);
}

bool Cloud::Export::NeedsStructTypes(const TSharedPtr<FJsonObject>& Response) {
	const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr;

	if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("exports"), Exports)) return false;

	TMap<const UStruct*, bool> Seen;

	for (const TSharedPtr<FJsonValue>& Value : *Exports) {
		const TSharedPtr<FJsonObject> Export = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		FString Type;

		if (!Export.IsValid() || !Export->TryGetStringField(TEXT("Type"), Type) || Type.IsEmpty()) continue;

		/* A class this editor doesn't have yet (a Blueprint's, before its import) could hold one as well */
		const UClass* Class = FindFirstObject<UClass>(*Type, EFindFirstObjectOptions::NativeFirst);

		if (Class == nullptr || HoldsTypedStructs(Class, Seen)) return true;
	}

	return false;
}

int32 Cloud::Export::AddStructTypes(const FString& Path, const TSharedPtr<FJsonObject>& Response, const TSharedPtr<FJsonObject>& Typed) {
	const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* TypedExports = nullptr;

	if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("exports"), Exports)) return 0;

	/* A Mirror without the typed export (upstream's) leaves the raw one as it was */
	if (!Typed.IsValid() || !Typed->TryGetArrayField(TEXT("exports"), TypedExports)) {
		UE_LOG(LogReflection, Verbose, TEXT("No typed export for \"%s\": its instanced structs stay unnamed"), *Path);

		return 0;
	}

	TMap<FString, TSharedPtr<FJsonObject>> ByKey;

	for (const TSharedPtr<FJsonValue>& Value : *TypedExports) {
		if (Value.IsValid() && Value->Type == EJson::Object) {
			ByKey.FindOrAdd(ExportKey(Value->AsObject()), Value->AsObject());
		}
	}

	int32 Named = 0;

	for (const TSharedPtr<FJsonValue>& Value : *Exports) {
		if (!Value.IsValid() || Value->Type != EJson::Object) continue;

		const TSharedPtr<FJsonObject> Export = Value->AsObject();
		const TSharedPtr<FJsonObject>* TypedExport = ByKey.Find(ExportKey(Export));
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		const TSharedPtr<FJsonObject>* TypedProperties = nullptr;

		if (TypedExport == nullptr || !Export->TryGetObjectField(TEXT("Properties"), Properties)
			|| !(*TypedExport)->TryGetObjectField(TEXT("Properties"), TypedProperties)) continue;

		NameStructs(MakeShared<FJsonValueObject>(*Properties), MakeShared<FJsonValueObject>(*TypedProperties), Named);
	}

	if (Named > 0) {
		UE_LOG(LogReflection, Log, TEXT("Named %d instanced struct(s) in \"%s\" from its typed export"), Named, *Path);
	}

	return Named;
}

/* Exports ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void Cloud::Export::GetAsync(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));

	if (!Raw) {
		Cloud::Get(ExportURL, Parameters, Headers, MoveTemp(OnResponse));

		return;
	}

	/* The typed export asked for second, only when the raw one can hold an instanced struct (AddStructTypes) */
	Cloud::Get(ExportURL, Parameters, Headers, [Path, OnResponse](const TSharedPtr<FJsonObject>& Response) {
		if (!NeedsStructTypes(Response)) {
			OnResponse(Response);

			return;
		}

		Cloud::Get(TypedExportURL, { { TEXT("path"), Path } }, {}, [Path, Response, OnResponse](const TSharedPtr<FJsonObject>& Typed) {
			AddStructTypes(Path, Response, Typed);
			OnResponse(Response);
		});
	});
}

void Cloud::Export::GetRawAsync(const FString& Path, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse) {
	GetAsync(Path, true, {}, {}, MoveTemp(OnResponse));
}

void Cloud::Export::GetRawExportsAsync(const FString& Path, TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnResponse) {
	GetRawAsync(Path, [OnResponse](const TSharedPtr<FJsonObject>& Response) {
		/* Unreachable Cloud, and a path the Cloud has no export for, are the same to a caller */
		if (!Response.IsValid() || Response->HasField(TEXT("errored"))) {
			OnResponse(TArray<TSharedPtr<FJsonValue>>());

			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Exports;

		if (!Response->TryGetArrayField(TEXT("exports"), Exports)) {
			OnResponse(TArray<TSharedPtr<FJsonValue>>());

			return;
		}

		OnResponse(*Exports);
	});
}

TSharedPtr<FJsonObject> Cloud::Export::GetBlocking(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(ExportURL, Parameters, Headers);

	/* The typed export asked for only when the raw one can hold an instanced struct (AddStructTypes) */
	if (Raw && NeedsStructTypes(Response)) {
		AddStructTypes(Path, Response, Cloud::GetBlocking(TypedExportURL, { { TEXT("path"), Path } }));
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetRawBlocking(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	return GetBlocking(Path, true, Parameters, Headers);
}

TArray<TSharedPtr<FJsonValue>> Cloud::Export::GetSkinWeightsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(SkinWeightsURL, { { TEXT("path"), Path } }, {});

	/* Unreachable, missing and profile-less all leave the caller with the same nothing */
	if (!Response.IsValid() || !Response->HasField(TEXT("profiles"))) {
		return TArray<TSharedPtr<FJsonValue>>();
	}

	return Response->GetArrayField(TEXT("profiles"));
}

TSharedPtr<FJsonObject> Cloud::Export::GetLodModelBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(LodModelURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("lods"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetCurveExpressionsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(ExpressionsURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("expressions"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetCurveMappingBlocking() {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(CurveMappingURL, {}, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("entries"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetDnaMorphsBlocking(const FString& Path, const bool bBackport) {
	TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

	/* Asked for rather than pointed at.
	 *
	 * Which mapping an older head's curves are written in terms of is a fact about the game, not a
	 * choice, so the Cloud knows it and this says only whether it is wanted. */
	if (bBackport) {
		Parameters.Add(TEXT("backport"), TEXT("true"));
	}

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(DnaMorphsURL, Parameters, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("morphs"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetDnaPosesBlocking(const FString& Path, const bool bBackport) {
	TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

	/* Asked for rather than pointed at, the same as the morphs */
	if (bBackport) {
		Parameters.Add(TEXT("backport"), TEXT("true"));
	}

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(DnaPosesURL, Parameters, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("poses"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetReferenceSkeletonBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(ReferenceSkeletonURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("bones"))) {
		return nullptr;
	}

	return Response;
}

/* Fortnite cooks its heads with a RigLogic newer than this engine's. That cook leaves the DNA's
 * behavior layer empty and bakes the rig into a RigLogic dump behind it, at a file version and with
 * layers this RigLogic has never seen, so the DNA as packaged reads as a face that has joints, a
 * neutral pose, and nothing to move them with.
 *
 * Everything the behavior layer held is still in that dump, only rearranged, so the Cloud can put
 * the layer back and write the result at a version this engine does read. Ask for that where the
 * engine needs it, and take the DNA as packaged where it does not.
 *
 * A rig in a shape the Cloud does not recognise is refused rather than guessed at, so a failure
 * falls back to the packaged DNA: a head that holds still beats no head. */
TArray<uint8> Cloud::Export::GetFontFaceBlocking(const FString& Path) {
	const FReflectionHttpRequest Request = BuildRequest(FontFaceURL, { { TEXT("path"), Path } }, {});
	Request->SetVerb(TEXT("GET"));

	const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return {};
	}

	/* Json means the Cloud answered about the asset rather than with a typeface */
	if (Response->GetContentType().StartsWith(TEXT("application/json"))) {
		return {};
	}

	return Response->GetContent();
}

TArray<uint8> Cloud::Export::GetDnaBlocking(const FString& Path) {
	/* A lambda rather than a free function so it keeps this member's access to BuildRequest */
	const auto Fetch = [&Path](const bool bRebuiltForThisEngine) -> TArray<uint8> {
		TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

		if (bRebuiltForThisEngine) {
			Parameters.Add(TEXT("legacy"), TEXT("true"));
		}

		const FReflectionHttpRequest Request = BuildRequest(DnaURL, Parameters, {});
		Request->SetVerb(TEXT("GET"));

		const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

		if (!Response.IsValid() || Response->GetResponseCode() != 200) {
			return {};
		}

		/* Json means the Cloud answered about the mesh rather than with a DNA */
		if (Response->GetContentType().StartsWith(TEXT("application/json"))) {
			return {};
		}

		return Response->GetContent();
	};

	/* The game is cooked by a newer RigLogic than any engine this builds against (UE6 for Fortnite
	 * 42.x), which moves the rig out of the DNA into a baked dump beside it: the DNA as packaged is
	 * the definition alone, with no joint groups, and a face reads it as a rig that does nothing. The
	 * rebuilt one has the behavior put back; for a DNA that never had it moved out it is the packaged
	 * one unchanged. This holds on 5.8 too, whose UDNA reads a DNA the same way. */
	if (TArray<uint8> Rebuilt = Fetch(true); Rebuilt.Num() != 0) {
		return Rebuilt;
	}

	UE_LOG(LogReflection, Warning,
		TEXT("The rig in \"%s\" could not be rebuilt for this engine, falling back to the DNA as packaged"), *Path);

	return Fetch(false);
}

TSharedPtr<FJsonObject> Cloud::Export::GetMorphTargetsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(MorphTargetsURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("morphs"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetStaticMeshBlocking(const FString& Path, const FString& Named) {
	TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

	if (!Named.IsEmpty()) {
		Parameters.Add(TEXT("export_name"), Named);
	}

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(StaticMeshURL, Parameters, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("lods"))) {
		return nullptr;
	}

	return Response;
}

bool Cloud::Export::GetBinaryBlocking(const FString& Path, const FString& ContentType, TArray<uint8>& OutData, const FString& Named) {
	TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

	if (!Named.IsEmpty()) {
		Parameters.Add(TEXT("export_name"), Named);
	}

	const FReflectionHttpRequest Request = BuildRequest(ExportURL, Parameters, { { TEXT("content-type"), ContentType } });
	Request->SetVerb(TEXT("GET"));

	const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return false;
	}

	/* Json here means the Cloud has the export but couldn't hand back the payload for it */
	if (Response->GetContentType().StartsWith(TEXT("application/json"))) {
		return false;
	}

	OutData = Response->GetContent();

	return OutData.Num() > 0;
}

/* Folders ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
TArray<FString> Cloud::Folder::GetPathsBlocking(const FString& Path) {
	TArray<FString> Paths;

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(FolderPathsURL, { { TEXT("path"), Path } });
	if (!Response.IsValid() || !Response->HasField(TEXT("paths"))) {
		return Paths;
	}

	for (const TSharedPtr<FJsonValue>& Value : Response->GetArrayField(TEXT("paths"))) {
		FString AssetPath;

		if (Value.IsValid() && Value->TryGetString(AssetPath) && !AssetPath.IsEmpty()) {
			Paths.Add(AssetPath);
		}
	}

	return Paths;
}

/* Metadata ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
static void ApplyMetadata(const TSharedPtr<FJsonObject>& MetadataResponse) {
	/* The project the files came out of, which is what every Cloud path is spelled with. Taken
	 * from the answer either way: keeping the last one is how a name from another profile ends up
	 * being applied to paths it has nothing to do with. */
	GReflectionRuntime.Profile.ProjectName = MetadataResponse->HasField(TEXT("name"))
		? MetadataResponse->GetStringField(TEXT("name"))
		: FString();

	if (MetadataResponse->HasField(TEXT("major_version"))) {
		const int MajorVersion = MetadataResponse->GetIntegerField(TEXT("major_version"));

		GReflectionRuntime.MajorVersion = MajorVersion;
	}

	if (MetadataResponse->HasField(TEXT("minor_version"))) {
		const int MinorVersion = MetadataResponse->GetIntegerField(TEXT("minor_version"));

		GReflectionRuntime.MinorVersion = MinorVersion;
	}

	/* Where a widget tree may have been cooked instead of in the widget itself. Mirror knows
		 * the build, so it is the half that names these. */
	GReflectionRuntime.WidgetTreeSources.Empty();

	if (const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr; MetadataResponse->TryGetArrayField(TEXT("widget_tree_sources"), Sources)) {
		for (const TSharedPtr<FJsonValue>& Value : *Sources) {
			if (!Value.IsValid()) continue;

			const FString Held = Value->AsString().TrimStartAndEnd();

			if (!Held.IsEmpty()) GReflectionRuntime.WidgetTreeSources.AddUnique(Held);
		}
	}

	if (MetadataResponse->HasField(TEXT("profile"))) {
		const auto Profile = MetadataResponse->GetObjectField(TEXT("profile"));

		GReflectionRuntime.Profile.Name = Profile->GetStringField(TEXT("name"));
	}
}

/* Asked every time rather than remembered. The Cloud can be pointed at a different profile between
 * one press and the next, and a project name held over from the last one stops matching the paths
 * coming down, which is enough to turn the project's own folder into a plugin. One request against
 * a local server, once per press. */
bool Cloud::EnsureMetadataBlocking() {
	/* Waited for rather than given up on at the first miss */
	const FBlockingRequestScope BlockingScope(NSLOCTEXT("Reflection", "AskingCloudMetadata", "Connecting to Cloud"));

	/* Long enough not to hammer a server that is still starting, short enough to feel immediate */
	constexpr double BetweenTriesSeconds = 0.25;

	for (;;) {
		if (const TSharedPtr<FJsonObject> Response = GetBlocking(MetadataURL); Response.IsValid()) {
			ApplyMetadata(Response);

			if (!GReflectionRuntime.Profile.ProjectName.IsEmpty()) {
				return true;
			}
		}

		/* The wait is the reader's to end, which is what the dialog's Cancel is for */
		const double Until = FPlatformTime::Seconds() + BetweenTriesSeconds;

		while (FPlatformTime::Seconds() < Until) {
			if (FBlockingRequestScope::Pump()) {
				return false;
			}

			/* Spent keeping the editor drawn rather than asleep */
			FPlatformProcess::Sleep(0.02f);
		}
	}
}

/* Builds ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void Cloud::Builds::ListAsync(TFunction<void(const TArray<FBuild>&)> OnResponse) {
	Get(BuildsURL, {}, {}, [OnResponse](const TSharedPtr<FJsonObject>& Response) {
		TArray<FBuild> Builds;

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

		if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("builds"), Listed)) {
			OnResponse(Builds);

			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Listed) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			const TSharedPtr<FJsonObject> Listing = Value->AsObject();

			FBuild Found;

			Listing->TryGetStringField(TEXT("name"), Found.Name);
			Listing->TryGetStringField(TEXT("id"), Found.Id);
			Listing->TryGetStringField(TEXT("versionName"), Found.Version);
			Listing->TryGetBoolField(TEXT("loaded"), Found.Loaded);
			Listing->TryGetBoolField(TEXT("isMain"), Found.Open);
			Listing->TryGetNumberField(TEXT("files"), Found.Files);

			/* The file is what a request names it by, so one without that cannot be acted on */
			if (Found.Id.IsEmpty()) continue;

			Builds.Add(Found);
		}

		OnResponse(Builds);
	});
}

/* Said the same way for all three: they differ only in what they ask Mirror to do */
static void PostToBuild(const FString& Id, const FString& What, TFunction<void(bool)> OnResponse) {
	Cloud::Post(Cloud::BuildsURL + TEXT("/") + Id + TEXT("/") + What, TEXT("{}"), {},
		[OnResponse](const TSharedPtr<FJsonObject>& Response, const int32 ResponseCode) {
			OnResponse(ResponseCode >= 200 && ResponseCode < 300);
		}
	);
}

void Cloud::Builds::UseAsync(const FString& Id, TFunction<void(bool)> OnResponse) {
	PostToBuild(Id, TEXT("use"), MoveTemp(OnResponse));
}

void Cloud::Builds::StartAsync(const FString& Id, TFunction<void(bool)> OnResponse) {
	PostToBuild(Id, TEXT("start"), MoveTemp(OnResponse));
}

void Cloud::Builds::StopAsync(const FString& Id, TFunction<void(bool)> OnResponse) {
	PostToBuild(Id, TEXT("stop"), MoveTemp(OnResponse));
}

void Cloud::Update(TFunction<void(bool)> OnResponse) {
	Get(MetadataURL, {}, {}, [OnResponse](const TSharedPtr<FJsonObject>& MetadataResponse) {
		if (!MetadataResponse.IsValid()) {
			OnResponse(false);

			return;
		}

		ApplyMetadata(MetadataResponse);

		OnResponse(true);
	});
}
