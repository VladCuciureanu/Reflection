// Importer regression checks for the local MaterialLab workflow.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ReflectionImporterAuditCommandlet.generated.h"

UCLASS()
class UReflectionImporterAuditCommandlet : public UCommandlet {
    GENERATED_BODY()
public:
    virtual int32 Main(const FString& Params) override;
};
