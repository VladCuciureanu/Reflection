/* Copyright Reflection Contributors 2024-2026 */
#pragma once
#include "Containers/Export.h"

// Material expressions are imported under the asset rather than their original
// graph outers. Preserve identity before flattening those outers. A leaf name
// alone cannot distinguish the reroutes in two different composite graphs.
inline int32 NormalizeMaterialExpressionIdentities(const TArray<FUObjectExport*>& Exports, const FString& PackagePath) {
    TMap<FName, TArray<FUObjectExport*>> Groups;
    TSet<FName> UsedNames;
    TMap<int32, FUObjectExport*> Positions;
    TMap<FString, FUObjectExport*> WholeNames;
    TMap<FUObjectExport*, FString> Originals;
    for (FUObjectExport* Export : Exports) {
        UsedNames.Add(Export->GetName());
        if (!Export->GetType().ToString().StartsWith(TEXT("MaterialExpression"))) continue;
        const FString Name = Export->GetName().ToString();
        Groups.FindOrAdd(Export->GetName()).Add(Export);
        Originals.Add(Export, Name);
        if (Export->Position >= 0) Positions.Add(Export->Position, Export);
        const TSharedPtr<FJsonObject>* Outer;
        FString OuterName;
        if (Export->JsonObject->TryGetObjectField(TEXT("Outer"), Outer) && (*Outer)->TryGetStringField(TEXT("ObjectName"), OuterName)) {
            const FString WholeOuter = StripObjectOuter(OuterName);
            const FString Whole = WholeOuter + (WholeOuter.Contains(TEXT(":")) ? TEXT(".") : TEXT(":")) + Name;
            // Ambiguous full identities must not resolve to whichever came last.
            if (WholeNames.Contains(Whole)) WholeNames[Whole] = nullptr;
            else WholeNames.Add(Whole, Export);
        }
    }
    TMap<FUObjectExport*, FString> Renames;
    for (const auto& Group : Groups) {
        if (Group.Value.Num() < 2) continue;
        for (FUObjectExport* Export : Group.Value) {
            FString Unique = FString::Printf(TEXT("%s__Export_%d"), *Originals[Export], Export->Position);
            while (UsedNames.Contains(FName(*Unique))) Unique += TEXT("_");
            UsedNames.Add(FName(*Unique));
            Renames.Add(Export, Unique);
        }
    }
    if (Renames.Num() == 0) return 0;
    TFunction<void(const TSharedPtr<FJsonValue>&)> Rewrite;
    Rewrite = [&](const TSharedPtr<FJsonValue>& Value) {
        if (!Value.IsValid()) return;
        if (Value->Type == EJson::Array) {
            for (const auto& Child : Value->AsArray()) Rewrite(Child);
        } else if (Value->Type == EJson::Object) {
            const auto Object = Value->AsObject();
            FString Named, Path, RefPackage, Index;
            FUObjectExport* Target = nullptr;
            // Restrict numeric positions AND whole-name matches to this package.
            if (Object->TryGetStringField(TEXT("ObjectName"), Named) && Object->TryGetStringField(TEXT("ObjectPath"), Path) &&
                Path.Split(TEXT("."), &RefPackage, &Index, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && RefPackage == PackagePath) {
                const FString Whole = StripObjectOuter(Named);
                if (FUObjectExport** Found = WholeNames.Find(Whole)) Target = *Found;
                if (!Target && Index.IsNumeric()) {
                    if (FUObjectExport** Found = Positions.Find(FCString::Atoi(*Index))) {
                        const FString ExpectedType = (*Found)->GetType().ToString() + TEXT("'");
                        // Editor/cooked exports can have different index orders.
                        // A qualified name must agree exactly, not just by leaf.
                        if (!Whole.Contains(TEXT(":")) && Named.StartsWith(ExpectedType) && Whole == Originals[*Found]) Target = *Found;
                    }
                }
                if (Target && Named.StartsWith(Target->GetType().ToString() + TEXT("'"))) {
                    if (const FString* Unique = Renames.Find(Target)) {
                        const int32 LeafAt = Named.Len() - Originals[Target].Len() - 1;
                        if (LeafAt >= 0 && Named.Mid(LeafAt, Originals[Target].Len()) == Originals[Target])
                            Object->SetStringField(TEXT("ObjectName"), Named.Left(LeafAt) + *Unique + TEXT("'"));
                    }
                }
            }
            // Existing material serializers also consult the cached leaf field.
            const TSharedPtr<FJsonObject>* Expression;
            if (Object->TryGetObjectField(TEXT("Expression"), Expression) && Object->HasField(TEXT("ExpressionName"))) {
                Rewrite(MakeShared<FJsonValueObject>(*Expression));
                FString ExpressionObjectName;
                if ((*Expression)->TryGetStringField(TEXT("ObjectName"), ExpressionObjectName))
                    Object->SetStringField(TEXT("ExpressionName"), GetObjectNameFromOuter(ExpressionObjectName));
            }
            for (const auto& Field : Object->Values) Rewrite(Field.Value);
        }
    };
    for (FUObjectExport* Export : Exports) Rewrite(MakeShared<FJsonValueObject>(Export->JsonObject));
    for (const auto& Rename : Renames) {
        Rename.Key->NameOverride = FName(*Rename.Value);
        Rename.Key->JsonObject->SetStringField(TEXT("Name"), Rename.Value);
    }
    return Renames.Num();
}
