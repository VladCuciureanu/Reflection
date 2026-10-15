#include "ReflectionImporterAuditCommandlet.h"
#include "Importers/Types/Texture/TextureFormats.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Utilities/AssetPaths.h"
#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "Engine/Plugin.h"
#include "Misc/PackageName.h"
#include "Containers/ExportContainer.h"
#include "Utilities/MaterialExpressionIdentity.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogImporterAudit, Log, All);

int32 UReflectionImporterAuditCommandlet::Main(const FString& Params) {
    int32 Failures = 0;
    auto Verify = [&](bool Passed, const TCHAR* What) {
        if (!Passed) { UE_LOG(LogImporterAudit, Error, TEXT("FAIL: %s"), What); ++Failures; }
        else { UE_LOG(LogImporterAudit, Display, TEXT("PASS: %s"), What); }
    };
    const FString CollisionJson = TEXT(R"JSON([
      {"Type":"Material","Name":"Audit","Properties":{}},
      {"Type":"MaterialExpressionReroute","Name":"MaterialExpressionReroute_0","Outer":{"ObjectName":"Material'Audit'","ObjectPath":"/Game/Audit.0"},"Properties":{}},
      {"Type":"MaterialExpressionReroute","Name":"MaterialExpressionReroute_0","Outer":{"ObjectName":"MaterialGraph'Audit:Nested'","ObjectPath":"/Game/Audit.4"},"Properties":{}},
      {"Type":"MaterialExpressionReroute","Name":"MaterialExpressionReroute_0__Export_1","Outer":{"ObjectName":"Material'Audit'","ObjectPath":"/Game/Audit.0"},"Properties":{}},
      {"Type":"MaterialGraph","Name":"Nested","Properties":{}},
      {"Type":"MaterialExpressionAdd","Name":"Caller","Properties":{
        "A":{"Expression":{"ObjectName":"MaterialExpressionReroute'Audit:MaterialExpressionReroute_0'","ObjectPath":"/Game/Audit.1"},"ExpressionName":"MaterialExpressionReroute_0"},
        "B":{"Expression":{"ObjectName":"MaterialExpressionReroute'Audit:Nested.MaterialExpressionReroute_0'","ObjectPath":"/Game/Audit.1"},"ExpressionName":"MaterialExpressionReroute_0"},
        "External":{"ObjectName":"MaterialExpressionReroute'Other:MaterialExpressionReroute_0'","ObjectPath":"/Game/Other.1"}
      }}
    ])JSON");
    TArray<TSharedPtr<FJsonValue>> CollisionExports;
    Verify(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CollisionJson), CollisionExports), TEXT("Expression collision fixture parsed"));
    FUObjectExportContainer CollisionContainer(CollisionExports);
    Verify(NormalizeMaterialExpressionIdentities(CollisionContainer.Exports, TEXT("/Game/Audit")) == 2, TEXT("Both colliding expressions receive distinct identities"));
    Verify(CollisionContainer.Exports[1]->GetName() == FName(TEXT("MaterialExpressionReroute_0__Export_1_")), TEXT("Generated expression names avoid existing names"));
    const auto Caller = CollisionContainer.Exports[5]->GetProperties();
    Verify(Caller->GetObjectField(TEXT("A"))->GetStringField(TEXT("ExpressionName")) == CollisionContainer.Exports[1]->GetName().ToString(), TEXT("Top-level connection retains exact target"));
    Verify(Caller->GetObjectField(TEXT("B"))->GetStringField(TEXT("ExpressionName")) == CollisionContainer.Exports[2]->GetName().ToString(), TEXT("Nested connection follows full name despite mismatched cooked index"));
    Verify(Caller->GetObjectField(TEXT("External"))->GetStringField(TEXT("ObjectName")) == TEXT("MaterialExpressionReroute'Other:MaterialExpressionReroute_0'"), TEXT("External package references remain unchanged"));
    Verify(NormalizeMaterialExpressionIdentities(CollisionContainer.Exports, TEXT("/Game/Audit")) == 0, TEXT("Expression identity normalization is idempotent"));
    for (FUObjectExport* Export : CollisionContainer.Exports) delete Export;
    CollisionContainer.Exports.Empty();
    FTextureSourceLayout Half, Full;
    Verify(FTextureFormats::GetLayout(PF_R16F, Half) && Half.SourceFormat == TSF_R16F && Half.GetEncodedSliceSize(3, 2) == 12 && Half.GetDecodedSliceSize(3, 2) == 12, TEXT("R16F native source layout"));
    Verify(FTextureFormats::GetLayout(PF_A32B32G32R32F, Full) && Full.SourceFormat == TSF_RGBA32F && Full.GetEncodedSliceSize(3, 2) == 96 && Full.GetDecodedSliceSize(3, 2) == 96, TEXT("RGBA32F native source layout"));
    const uint16 HalfPixels[] = {0x3c00, 0xc000, 0x0001, 0x8000, 0x7bff, 0x3555};
    uint16 HalfCopy[6] = {};
    FTextureFormats::Decode(reinterpret_cast<const uint8*>(HalfPixels), reinterpret_cast<uint8*>(HalfCopy), 3, 2, sizeof(HalfPixels), PF_R16F);
    Verify(FMemory::Memcmp(HalfPixels, HalfCopy, sizeof(HalfPixels)) == 0, TEXT("Half-float bits preserved including negative and subnormal values"));
    const float Pixels[] = {-2.0f, 0.25f, 3.0f, 0.5f};
    float Copy[4] = {};
    FTextureFormats::Decode(reinterpret_cast<const uint8*>(Pixels), reinterpret_cast<uint8*>(Copy), 1, 1, sizeof(Pixels), PF_A32B32G32R32F);
    Verify(FMemory::Memcmp(Pixels, Copy, sizeof(Pixels)) == 0, TEXT("Full-float RGBA channels preserved without clipping"));
    Verify(ToEditorPackagePath(TEXT("/Volumetrics/Content/PlanetPrototype/Functions/WorldAlignedNormal_HQ")) == TEXT("/Volumetrics/PlanetPrototype/Functions/WorldAlignedNormal_HQ"), TEXT("Engine-plugin Content alias normalized"));
    Verify(ToEditorPackagePath(TEXT("/Game/Keep/Content/ActualFolder")) == TEXT("/Game/Keep/Content/ActualFolder"), TEXT("Real Content folder preserved"));
    FString RootsFile;
    if (FParse::Value(*Params, TEXT("roots="), RootsFile)) {
        TArray<FString> Roots;
        Verify(FFileHelper::LoadFileToStringArray(Roots, *RootsFile), TEXT("Audit roots loaded"));
        for (FString Root : Roots) {
            Root.TrimStartAndEndInline();
            if (Root.IsEmpty()) continue;
            UObject* Asset = LoadObject<UObject>(nullptr, *ToEditorPackagePath(Root));
            Verify(Asset != nullptr, *FString::Printf(TEXT("Saved asset reloads: %s"), *Root));
            if (UMaterialFunctionInstance* Instance = Cast<UMaterialFunctionInstance>(Asset)) {
                Verify(Instance->Parent != nullptr && Instance->GetBaseFunction() != nullptr, TEXT("Function instance parent/base resolved"));
                UE_LOG(LogImporterAudit, Display, TEXT("%s: scalar=%d vector=%d texture=%d switches=%d"), *Root, Instance->ScalarParameterValues.Num(), Instance->VectorParameterValues.Num(), Instance->TextureParameterValues.Num(), Instance->StaticSwitchParameterValues.Num());
                for (const FTextureParameterValue& Parameter : Instance->TextureParameterValues) Verify(Parameter.ParameterValue != nullptr, TEXT("Function instance texture override resolves"));
                if (Instance->GetName() == TEXT("MF_CrewStyle_Ice_Inst")) {
                    Verify(Instance->ScalarParameterValues.Num() == 14 && Instance->VectorParameterValues.Num() == 3 && Instance->TextureParameterValues.Num() == 2 && Instance->StaticSwitchParameterValues.Num() == 4, TEXT("Ice instance override counts match native export"));
                    const FScalarParameterValue* Scalar = Instance->ScalarParameterValues.FindByPredicate([](const FScalarParameterValue& P) { return P.ParameterInfo.Name == TEXT("Silk Fresnel"); });
                    Verify(Scalar && Scalar->ParameterValue == 5.0f, TEXT("Scalar override retains native value"));
                    const FStaticSwitchParameter* Switch = Instance->StaticSwitchParameterValues.FindByPredicate([](const FStaticSwitchParameter& P) { return P.ParameterInfo.Name == TEXT("UseSilk"); });
                    Verify(Switch && Switch->Value && Switch->bOverride, TEXT("Static switch retains native value and override flag"));
                }
            }
            if (UTexture2D* Texture = Cast<UTexture2D>(Asset)) {
                if (Texture->GetName() == TEXT("T_Binary_Visor_DF")) Verify(Texture->Source.GetFormat() == TSF_R16F && Texture->Source.GetSizeX() > 0, TEXT("Saved visor texture retains R16F source"));
                if (Texture->GetName() == TEXT("T_DiscoFX_Pattern")) Verify(Texture->Source.GetFormat() == TSF_RGBA32F && Texture->Source.GetSizeX() > 0, TEXT("Saved disco texture retains RGBA32F source"));
            }
        }
    }
    return Failures ? 1 : 0;
}
