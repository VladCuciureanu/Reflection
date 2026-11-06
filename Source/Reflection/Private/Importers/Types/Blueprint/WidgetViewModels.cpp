/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/WidgetViewModels.h"

#include "Containers/ExportContainer.h"
#include "Engine/Log.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Serializers/ObjectSerializer.h"
#include "WidgetBlueprint.h"

#if REFLECTION_MVVM
#include "Bindings/MVVMConversionFunctionHelper.h"
#include "Components/Widget.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMBlueprintPin.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMDeveloperProjectSettings.h"
#include "MVVMPropertyPath.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "Types/MVVMExecutionMode.h"
#include "Types/MVVMFieldVariant.h"
#include "View/MVVMViewModelContextResolver.h"
#endif

namespace {
	/* Said in the issues panel, and in the log, where a headless import can be read back */
	void Say(const EImportIssue Kind, const FString& Summary, const FString& Detail) {
		FImportIssues::Report(Kind, Summary, Detail);

		UE_LOG(LogReflection, Warning, TEXT("%s: %s"), *Summary, *Detail);
	}

	/* FMVVMViewClass_Source::EFlags, as the compiler writes them */
	enum ESourceFlags : uint16 {
		TypeCreateInstance = 1 << 0,
		IsUserWidgetProperty = 1 << 1,
		IsOptional = 1 << 3,
		CanBeSet = 1 << 4,
		SelfReference = 1 << 7,
		IsViewModel = 1 << 9,
		IsViewModelInstanceExposed = 1 << 10,
		GlobalViewModelCollectionUpdate = 1 << 11,
		AlwaysExecuteBindingsOnSetSource = 1 << 12,
		ParentContextReference = 1 << 13
	};

	/* FMVVMViewClass_Binding::EFlags */
	enum EBindingFlags : uint8 {
		OneWay = 1 << 0,
		OverrideExecuteMode = 1 << 2,
		EnabledByDefault = 1 << 3
	};

	/* FMVVMVCompiledBinding::EType */
	enum class EBindingType : uint8 {
		None = 0,
		HasConversionFunction = 1,
		HasComplexConversionFunction = 2,
		BindingComplex = 3
	};

	/* What a source is: a viewmodel the blueprint holds a property for, rather than one of its widgets, itself, or one
	 * the compiler made for a path that runs through a viewmodel */
	bool IsBlueprintViewModel(const TSharedPtr<FJsonObject>& Source) {
		const int32 Flags = static_cast<int32>(Source->GetNumberField(TEXT("Flags")));

		return (Flags & IsViewModel) != 0 && (Flags & IsUserWidgetProperty) != 0;
	}

	/* The name inside a reference's quotes, past whatever it is outered to: Class'FortItemVM' is FortItemVM,
	 * FortGlobalVMResolver'WBP_C:MVVMViewClass_0.FortGlobalVMResolver_2' is FortGlobalVMResolver_2 */
	FString QuotedName(const FString& ObjectName) {
		FString Name = ObjectName;

		if (int32 Quote; Name.FindChar(TEXT('\''), Quote)) {
			Name = Name.Mid(Quote + 1);
			Name.RemoveFromEnd(TEXT("'"));
		}

		if (int32 At; Name.FindLastChar(TEXT(':'), At)) Name = Name.Mid(At + 1);
		if (int32 At; Name.FindLastChar(TEXT('.'), At)) Name = Name.Mid(At + 1);

		return Name;
	}

	bool IsCall(const FString& Token) {
		return Token == TEXT("EX_LocalVirtualFunction") || Token == TEXT("EX_LocalFinalFunction")
			|| Token == TEXT("EX_VirtualFunction") || Token == TEXT("EX_FinalFunction") || Token == TEXT("EX_CallMath");
	}

	/* The local or member an EX_*Variable names */
	FString VariableName(const TSharedPtr<FJsonObject>& Expression) {
		const TSharedPtr<FJsonObject>* Variable;

		if (!Expression.IsValid() || !Expression->TryGetObjectField(TEXT("Variable"), Variable)) return FString();

		if (const TSharedPtr<FJsonObject>* Property; (*Variable)->TryGetObjectField(TEXT("Property"), Property)) {
			return (*Property)->GetStringField(TEXT("Name"));
		}

		if (const TArray<TSharedPtr<FJsonValue>>* Path; (*Variable)->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
			return Path->Last()->AsString();
		}

		return FString();
	}
}

FWidgetViewModels::FWidgetViewModels(FUObjectExportContainer* InContainer, const TSharedPtr<FJsonObject>& ClassExport)
	: Container(InContainer) {
	if (Container == nullptr || !ClassExport.IsValid()) return;

	FString ClassName;

	if (!ClassExport->TryGetStringField(TEXT("Name"), ClassName)) return;

	/* The one view the class carries, outered to it */
	for (const FUObjectExport* Export : Container->Exports) {
		if (Export == nullptr || !Export->IsJsonValid() || Export->GetType() != TEXT("MVVMViewClass")) continue;

		const TSharedPtr<FJsonObject>* Outer;

		if (!Export->JsonObject->TryGetObjectField(TEXT("Outer"), Outer)) continue;
		if (QuotedName((*Outer)->GetStringField(TEXT("ObjectName"))) != ClassName) continue;

		const TSharedPtr<FJsonObject>* Written;

		if (Export->JsonObject->TryGetObjectField(TEXT("Properties"), Written)) View = *Written;

		break;
	}

	if (!View.IsValid()) return;

	/* The library loads every class's properties and then its functions, each into one list, and a field path names
	 * an entry in one of the two */
	if (const TSharedPtr<FJsonObject>* Library; View->TryGetObjectField(TEXT("BindingLibrary"), Library)) {
		const TArray<TSharedPtr<FJsonValue>> Names = (*Library)->GetArrayField(TEXT("CompiledFieldNames"));

		for (const TSharedPtr<FJsonValue>& Value : (*Library)->GetArrayField(TEXT("CompiledFields"))) {
			const TSharedPtr<FJsonObject> Fields = Value->AsObject();
			const TSharedPtr<FJsonObject> Owner = Fields->GetObjectField(TEXT("ClassOrScriptStruct"));
			const int32 Start = static_cast<int32>(Fields->GetNumberField(TEXT("LibraryStartIndex")));
			const int32 NumProperties = static_cast<int32>(Fields->GetNumberField(TEXT("NumberOfProperties")));
			const int32 NumFunctions = static_cast<int32>(Fields->GetNumberField(TEXT("NumberOfFunctions")));

			for (int32 Index = 0; Index < NumProperties + NumFunctions; ++Index) {
				FField Field;
				Field.bProperty = Index < NumProperties;
				Field.Owner = Owner;
				Field.Name = Names.IsValidIndex(Start + Index) ? FName(*Names[Start + Index]->AsString()) : NAME_None;

				(Field.bProperty ? LoadedProperties : LoadedFunctions).Add(Field);
			}
		}

		FieldPaths = (*Library)->GetArrayField(TEXT("FieldPaths"));
	}

	/* What compiling the view makes of its own, which the rest of the import leaves to it */
	for (const TSharedPtr<FJsonValue>& Value : View->GetArrayField(TEXT("Sources"))) {
		const TSharedPtr<FJsonObject> Source = Value->AsObject();

		if (!IsBlueprintViewModel(Source)) continue;

		const FString Name = Source->GetStringField(TEXT("PropertyName"));
		Properties.Add(Name);

		/* The setter a viewmodel that can be set is given */
		if ((static_cast<int32>(Source->GetNumberField(TEXT("Flags"))) & CanBeSet) != 0) {
			Functions.Add(TEXT("Set") + Name);
		}
	}

	for (const TSharedPtr<FJsonValue>& Value : View->GetArrayField(TEXT("Bindings"))) {
		const TSharedPtr<FJsonObject> Compiled = Value->AsObject()->GetObjectField(TEXT("Binding"));
		const TArray<FField> Conversion = Path(Compiled->GetObjectField(TEXT("ConversionFunctionFieldPath")));

		/* A conversion that needed a function of its own, named after the binding */
		if (static_cast<EBindingType>(Compiled->GetNumberField(TEXT("Type"))) == EBindingType::HasComplexConversionFunction && Conversion.Num() > 0) {
			Functions.Add(Conversion.Last().Name.ToString());
		}
	}

	/* And a setter written for a destination native code cannot set */
	for (const FUObjectExport* Export : Container->Exports) {
		if (Export != nullptr && Export->IsJsonValid() && Export->GetType() == TEXT("Function") && Export->GetName().ToString().StartsWith(TEXT("__Setter_"))) {
			Functions.Add(Export->GetName().ToString());
		}
	}
}

bool FWidgetViewModels::IsValid() const {
#if REFLECTION_MVVM
	return View.IsValid();
#else
	return false;
#endif
}

TArray<FWidgetViewModels::FField> FWidgetViewModels::Path(const TSharedPtr<FJsonObject>& CompiledFieldPath) const {
	TArray<FField> Fields;

	if (!CompiledFieldPath.IsValid()) return Fields;

	const int32 Start = static_cast<int32>(CompiledFieldPath->GetNumberField(TEXT("StartIndex")));
	const int32 Num = static_cast<int32>(CompiledFieldPath->GetNumberField(TEXT("Num")));

	for (int32 Index = Start; Start >= 0 && Index < Start + Num && FieldPaths.IsValidIndex(Index); ++Index) {
		const TSharedPtr<FJsonObject> Entry = FieldPaths[Index]->AsObject();
		const int32 Loaded = static_cast<int32>(Entry->GetNumberField(TEXT("Index")));
		const TArray<FField>& From = Entry->GetBoolField(TEXT("bIsProperty")) ? LoadedProperties : LoadedFunctions;

		if (From.IsValidIndex(Loaded)) Fields.Add(From[Loaded]);
	}

	return Fields;
}

#if REFLECTION_MVVM

namespace {
	/* The class or struct a reference names: the blueprint's own skeleton for its own class, since what is being
	 * built is only on that one until it compiles again */
	UStruct* ResolveOwner(const TSharedPtr<FJsonObject>& Reference, const UWidgetBlueprint* Blueprint) {
		if (!Reference.IsValid()) return nullptr;

		const FString Name = QuotedName(Reference->GetStringField(TEXT("ObjectName")));

		if (Blueprint->GeneratedClass != nullptr && Name == Blueprint->GeneratedClass->GetName()) {
			return Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
		}

		/* A module for a native one, the package and the export's index for anything else */
		FString Package = Reference->GetStringField(TEXT("ObjectPath"));

		if (!Package.StartsWith(TEXT("/Script/"))) {
			if (int32 Dot; Package.FindLastChar(TEXT('.'), Dot)) Package.LeftInline(Dot);
		}

		if (UStruct* Found = LoadObject<UStruct>(nullptr, *(Package + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet)) return Found;

		return FindFirstObject<UStruct>(*Name, EFindFirstObjectOptions::NativeFirst);
	}

	/* What a field hands on to the next one along a path */
	UStruct* TypeOf(const UE::MVVM::FMVVMConstFieldVariant& Field) {
		const FProperty* Property = Field.IsProperty() ? Field.GetProperty() : (Field.IsFunction() ? Field.GetFunction()->GetReturnProperty() : nullptr);

		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property)) return Object->PropertyClass;
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property)) return Struct->Struct;
		if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property)) return Interface->InterfaceClass;

		return nullptr;
	}

	UE::MVVM::FMVVMConstFieldVariant FindField(const UStruct* Owner, const bool bProperty, const FName Name) {
		if (Owner == nullptr) return UE::MVVM::FMVVMConstFieldVariant();

		if (bProperty) {
			if (const FProperty* Property = FindFProperty<FProperty>(Owner, Name)) return UE::MVVM::FMVVMConstFieldVariant(Property);
		} else if (const UClass* Class = Cast<UClass>(Owner)) {
			if (const UFunction* Function = Class->FindFunctionByName(Name)) return UE::MVVM::FMVVMConstFieldVariant(Function);
		}

		return UE::MVVM::FMVVMConstFieldVariant();
	}

	/* Whether the view compiler sees a widget of this name: in the blueprint's own tree, or in the nearest parent's
	 * where the blueprint has none (FMVVMViewBlueprintCompiler::CreateWidgetMap) */
	bool SeesWidget(const UWidgetBlueprint* Blueprint, const FName Name) {
		for (const UWidgetBlueprint* Scan = Blueprint; Scan != nullptr;) {
			const TArray<const UWidget*> Widgets = Scan->GetAllSourceWidgets();

			if (Widgets.Num() > 0) return Widgets.ContainsByPredicate([Name](const UWidget* Widget) { return Widget != nullptr && Widget->GetFName() == Name; });

			Scan = Scan->ParentClass != nullptr ? Cast<UWidgetBlueprint>(Scan->ParentClass->ClassGeneratedBy) : nullptr;
		}

		return false;
	}

	/* One step of a path read out of bytecode, which says what it is on where the script does */
	struct FStep {
		bool bProperty = true;
		TSharedPtr<FJsonObject> Owner;
		FName Name;
	};

	/* An argument read as the path it reads: members and getters, each on whatever came before, rooted at the
	 * blueprint itself. A local stands for the path that was put in it. False for anything else, a literal. */
	bool ReadPath(const TSharedPtr<FJsonObject>& Expression, const TMap<FString, TArray<FStep>>& Locals, TArray<FStep>& Out) {
		if (!Expression.IsValid()) return false;

		const FString Token = Expression->GetStringField(TEXT("Token"));

		if (Token == TEXT("EX_Self")) return true;

		if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent")) {
			return ReadPath(Expression->GetObjectField(TEXT("ObjectExpression")), Locals, Out)
				&& ReadPath(Expression->GetObjectField(TEXT("ContextExpression")), Locals, Out);
		}

		if (Token == TEXT("EX_LocalVariable") || Token == TEXT("EX_LocalOutVariable")) {
			const TArray<FStep>* Put = Locals.Find(VariableName(Expression));

			if (Put == nullptr) return false;

			Out.Append(*Put);

			return true;
		}

		if (Token == TEXT("EX_InstanceVariable")) {
			FStep Step;
			Step.Name = FName(*VariableName(Expression));

			const TSharedPtr<FJsonObject> Variable = Expression->GetObjectField(TEXT("Variable"));

			if (const TSharedPtr<FJsonObject>* Owner; Variable->TryGetObjectField(TEXT("Owner"), Owner) || Variable->TryGetObjectField(TEXT("ResolvedOwner"), Owner)) {
				Step.Owner = *Owner;
			}

			Out.Add(Step);

			return true;
		}

		/* A getter: a call that takes nothing, made on whatever came before */
		if (IsCall(Token) && Expression->GetArrayField(TEXT("Parameters")).Num() == 0) {
			FStep Step;
			Step.bProperty = false;

			if (const TSharedPtr<FJsonObject>* Function; Expression->TryGetObjectField(TEXT("Function"), Function)) {
				FString Whole = (*Function)->GetStringField(TEXT("ObjectName"));

				/* Class'FortItemSelectionVM:GetPreviewItem' is GetPreviewItem on Class'FortItemSelectionVM' */
				if (FString Owner, Name; Whole.Split(TEXT(":"), &Owner, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
					Step.Name = FName(*Name.LeftChop(1));
					Step.Owner = MakeShared<FJsonObject>();
					Step.Owner->SetStringField(TEXT("ObjectName"), Owner + TEXT("'"));
					Step.Owner->SetStringField(TEXT("ObjectPath"), (*Function)->GetStringField(TEXT("ObjectPath")));
				}
			} else {
				Step.Name = FName(*Expression->GetStringField(TEXT("Function")));
			}

			Out.Add(Step);

			return true;
		}

		return false;
	}

	/* The call a conversion's function makes, read out of its bytecode */
	struct FConversionCall {
		TSharedPtr<FJsonObject> Call;
		TMap<FString, TArray<FStep>> Locals;
	};

	/* The one call in the function that is not a step of a path, and whose result reaches the function's out parameter.
	 *
	 * The compiler writes the function as each argument's path read into a local, then the call, then its result
	 * copied out. A call made on another object is a step of a path; one made on the blueprint or a library is the
	 * conversion. */
	bool ReadConversion(const TSharedPtr<FJsonObject>& Function, FConversionCall& Out) {
		TSet<FString> Outs;

		for (const TSharedPtr<FJsonValue>& Value : Function->GetArrayField(TEXT("ChildProperties"))) {
			FString Flags;

			if (Value->AsObject()->TryGetStringField(TEXT("PropertyFlags"), Flags) && Flags.Contains(TEXT("OutParm"))) {
				Outs.Add(Value->AsObject()->GetStringField(TEXT("Name")));
			}
		}

		TArray<TPair<TSharedPtr<FJsonObject>, FString>> Calls;
		FString Result;

		for (const TSharedPtr<FJsonValue>& Value : Function->GetArrayField(TEXT("ScriptBytecode"))) {
			const TSharedPtr<FJsonObject> Statement = Value->AsObject();
			const FString Token = Statement->GetStringField(TEXT("Token"));

			if (Token.StartsWith(TEXT("EX_Let"))) {
				const FString Into = VariableName(Statement->GetObjectField(TEXT("Variable")));
				const TSharedPtr<FJsonObject> Expression = Statement->GetObjectField(TEXT("Expression"));
				const FString Is = Expression->GetStringField(TEXT("Token"));

				if (IsCall(Is) && Expression->GetArrayField(TEXT("Parameters")).Num() > 0) {
					Calls.Emplace(Expression, Into);
				} else if (Outs.Contains(Into)) {
					if (Is == TEXT("EX_LocalVariable") || Is == TEXT("EX_LocalOutVariable")) Result = VariableName(Expression);
				} else if (TArray<FStep> Steps; ReadPath(Expression, Out.Locals, Steps)) {
					Out.Locals.Add(Into, Steps);
				}
			} else if (IsCall(Token)) {
				Calls.Emplace(Statement, FString());
			}
		}

		for (const TPair<TSharedPtr<FJsonObject>, FString>& Call : Calls) {
			bool bReaches = Outs.Contains(Call.Value) || (!Result.IsEmpty() && Call.Value == Result);

			for (const TSharedPtr<FJsonValue>& Parameter : Call.Key->GetArrayField(TEXT("Parameters"))) {
				if (!Result.IsEmpty() && VariableName(Parameter->AsObject()) == Result) bReaches = true;
			}

			if (bReaches) {
				Out.Call = Call.Key;

				return true;
			}
		}

		return false;
	}

	/* A literal as the default a pin is given */
	bool SetDefault(UEdGraphPin* Pin, const TSharedPtr<FJsonObject>& Literal) {
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		const FString Token = Literal->GetStringField(TEXT("Token"));

		if (Token == TEXT("EX_True") || Token == TEXT("EX_False")) {
			Schema->TrySetDefaultValue(*Pin, Token == TEXT("EX_True") ? TEXT("true") : TEXT("false"));

			return true;
		}

		if (Token == TEXT("EX_TextConst")) {
			FString Source;
			const TSharedPtr<FJsonObject>* Value;

			if (Literal->TryGetObjectField(TEXT("Value"), Value)) (*Value)->TryGetStringField(TEXT("SourceString"), Source);

			Schema->TrySetDefaultText(*Pin, FText::FromString(Source));

			return true;
		}

		const TSharedPtr<FJsonValue> Value = Literal->TryGetField(TEXT("Value"));

		if (!Value.IsValid()) return Token == TEXT("EX_NoObject");

		if (Value->Type == EJson::Number) {
			/* A byte given to an enum pin is that enum's value, which the pin spells by name */
			if (const UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get())) {
				Schema->TrySetDefaultValue(*Pin, Enum->GetNameStringByValue(static_cast<int64>(Value->AsNumber())));
			} else if (Token == TEXT("EX_FloatConst") || Token == TEXT("EX_DoubleConst")) {
				Schema->TrySetDefaultValue(*Pin, LexToSanitizedString(Value->AsNumber()));
			} else {
				Schema->TrySetDefaultValue(*Pin, FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber())));
			}

			return true;
		}

		if (Value->Type == EJson::String) {
			Schema->TrySetDefaultValue(*Pin, Value->AsString());

			return true;
		}

		return false;
	}
}

int32 FWidgetViewModels::ConstructViewModels(UWidgetBlueprint* Blueprint, UObjectSerializer* Serializer) {
	if (!IsValid() || Blueprint == nullptr) return 0;

	UMVVMWidgetBlueprintExtension_View* Extension = UMVVMWidgetBlueprintExtension_View::RequestExtension<UMVVMWidgetBlueprintExtension_View>(Blueprint);

	if (Extension->GetBlueprintView() == nullptr) Extension->CreateBlueprintViewInstance();

	UMVVMBlueprintView* BlueprintView = Extension->GetBlueprintView();

	if (BlueprintView == nullptr) return 0;

	/* A blueprint reflected again starts its view again, and drops what an import before this one made of what the
	 * view makes itself */
	{
		TArray<FGuid> Standing;

		for (const FMVVMBlueprintViewModelContext& Context : BlueprintView->GetViewModels()) Standing.Add(Context.GetViewModelId());

		BlueprintView->RemoveViewModels(Standing);

		while (BlueprintView->GetNumBindings() > 0) BlueprintView->RemoveBindingAt(0);

		for (const FString& Name : Properties) {
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) != INDEX_NONE) {
				FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Name));
			}
		}

		for (int32 At = Blueprint->FunctionGraphs.Num() - 1; At >= 0; --At) {
			UEdGraph* Graph = Blueprint->FunctionGraphs[At];

			if (Graph != nullptr && Functions.Contains(Graph->GetName())) {
				FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
			}
		}
	}

	ViewModels.Reset();

	/* The view's own settings, which the compile copies onto the view class under the same names: when it initializes
	 * itself, and whether it is made at all without a binding. The cook writes the ones that differ from the default. */
	if (UMVVMBlueprintViewSettings* Settings = BlueprintView->GetSettings()) {
		for (TFieldIterator<FBoolProperty> It(Settings->GetClass()); It; ++It) {
			if (bool bSet; View->TryGetBoolField(It->GetName(), bSet)) It->SetPropertyValue_InContainer(Settings, bSet);
		}
	}

	const bool bForcedOnSet = GetDefault<UMVVMDeveloperProjectSettings>()->bForceExecuteBindingsOnSetSource;
	int32 Added = 0;

	for (const TSharedPtr<FJsonValue>& Value : View->GetArrayField(TEXT("Sources"))) {
		const TSharedPtr<FJsonObject> Source = Value->AsObject();

		if (!IsBlueprintViewModel(Source)) continue;

		const FName Name(*Source->GetStringField(TEXT("PropertyName")));
		const int32 Flags = static_cast<int32>(Source->GetNumberField(TEXT("Flags")));
		const UClass* Class = Cast<UClass>(ResolveOwner(Source->GetObjectField(TEXT("ExpectedSourceType")), Blueprint));

		if (Class == nullptr) {
			Say(EImportIssue::MissingClass,
				FString::Printf(TEXT("The viewmodel \"%s\" has no class"), *Name.ToString()),
				FString::Printf(TEXT("%s names a viewmodel class this project doesn't have, so the viewmodel and its bindings were left out."), *Blueprint->GetName()));

			continue;
		}

		FMVVMBlueprintViewModelContext Context(Class, Name);

		/* How it was got hold of, which is the one flag or field of the source the compiler set from it */
		const TSharedPtr<FJsonObject>* Resolver = nullptr;
		const TSharedPtr<FJsonObject> Global = Source->GetObjectField(TEXT("GlobalViewModelInstance"));
		const TArray<FField> FieldPath = Path(Source->GetObjectField(TEXT("FieldPath")));

		if ((Flags & TypeCreateInstance) != 0) {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
			Context.bExposeInstanceInEditor = (Flags & IsViewModelInstanceExposed) != 0;
		} else if (Source->TryGetObjectField(TEXT("Resolver"), Resolver) && Resolver->IsValid()) {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::Resolver;

			const FUObjectExport* Made = Container->GetExportByObjectPath(*Resolver);
			const FString ResolverClassName = Made != nullptr && Made->IsJsonValid() ? Made->GetType().ToString() : QuotedName((*Resolver)->GetStringField(TEXT("ObjectName")));
			UClass* ResolverClass = FindFirstObject<UClass>(*ResolverClassName, EFindFirstObjectOptions::NativeFirst);

			if (ResolverClass == nullptr || !ResolverClass->IsChildOf(UMVVMViewModelContextResolver::StaticClass())) {
				Say(EImportIssue::MissingClass,
					FString::Printf(TEXT("The viewmodel \"%s\" is resolved by %s, which this project doesn't have"), *Name.ToString(), *ResolverClassName),
					TEXT("The viewmodel was kept with no resolver, which the view's compile reports."));
			} else {
				Context.Resolver = NewObject<UMVVMViewModelContextResolver>(BlueprintView, ResolverClass, NAME_None, RF_Transactional);

				if (Serializer != nullptr && Made != nullptr && Made->IsJsonValid() && Made->JsonObject->HasTypedField<EJson::Object>(TEXT("Properties"))) {
					Serializer->DeserializeObjectProperties(Made->JsonObject->GetObjectField(TEXT("Properties")), Context.Resolver);
				}
			}
		} else if (Global.IsValid() && Global->HasTypedField<EJson::Object>(TEXT("ContextClass"))) {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection;
			Context.GlobalViewModelIdentifier = FName(*Global->GetStringField(TEXT("ContextName")));
			Context.bGlobalViewModelCollectionUpdate = (Flags & GlobalViewModelCollectionUpdate) != 0;
		} else if ((Flags & ParentContextReference) != 0) {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::Context;
		} else if (FieldPath.Num() > 0) {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::PropertyPath;

			TArray<FString> Steps;

			for (const FField& Field : FieldPath) Steps.Add(Field.Name.ToString());

			Context.ViewModelPropertyPath = FString::Join(Steps, TEXT("."));
		} else {
			Context.CreationType = EMVVMBlueprintViewModelContextCreationType::Manual;
		}

		Context.bOptional = (Flags & IsOptional) != 0;
		Context.bCreateSetterFunction = (Flags & CanBeSet) != 0;

		/* Only set when it can be set, and the project's own setting where nobody said otherwise */
		const bool bOnSet = (Flags & AlwaysExecuteBindingsOnSetSource) != 0;

		if (Context.bCreateSetterFunction && bOnSet != bForcedOnSet) {
			Context.bOverrideForceExecuteBindingsOnSetSource = true;
			Context.bForceExecuteBindingsOnSetSource = bOnSet;
		}

		BlueprintView->AddViewModel(Context);
		ViewModels.Add(Name, Context.GetViewModelId());
		Added++;
	}

	if (Added > 0) {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return Added;
}

int32 FWidgetViewModels::ConstructBindings(UWidgetBlueprint* Blueprint) {
	if (!IsValid() || Blueprint == nullptr) return 0;

	UMVVMWidgetBlueprintExtension_View* Extension = UMVVMWidgetBlueprintExtension_View::GetExtension<UMVVMWidgetBlueprintExtension_View>(Blueprint);
	UMVVMBlueprintView* BlueprintView = Extension != nullptr ? Extension->GetBlueprintView() : nullptr;

	if (BlueprintView == nullptr) return 0;

	/* Asked each time rather than kept: making a conversion function can have the skeleton made again, and the
	 * old one goes with every field that was found on it */
	auto SelfClass = [Blueprint]() -> const UClass* {
		return Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
	};

	/* A path on the blueprint as the editor keeps one: where it starts, a viewmodel, a widget or the blueprint itself,
	 * and then each field along it. Steps that say what they are on are looked for there; the rest on what the step
	 * before handed on. */
	auto MakePath = [this, Blueprint, BlueprintView, SelfClass](const TArray<FStep>& Steps, FMVVMBlueprintPropertyPath& Out, FString& Missing, const bool bOnSelf = false) {
		Out = FMVVMBlueprintPropertyPath();

		int32 First = 0;
		const UClass* Self = SelfClass();
		const UStruct* On = Self;

		if (Steps.Num() > 0 && Steps[0].bProperty && !bOnSelf) {
			const UE::MVVM::FMVVMConstFieldVariant Root = FindField(Self, true, Steps[0].Name);
			const FObjectPropertyBase* Object = Root.IsProperty() ? CastField<FObjectPropertyBase>(Root.GetProperty()) : nullptr;

			if (const FGuid* ViewModel = ViewModels.Find(Steps[0].Name)) {
				Out.SetViewModelId(*ViewModel);

				/* The viewmodel's own class, which the property is declared as once the view has compiled */
				const FMVVMBlueprintViewModelContext* Context = BlueprintView->FindViewModel(*ViewModel);
				On = Context != nullptr ? Context->GetViewModelClass() : (Object != nullptr ? Object->PropertyClass.Get() : nullptr);
				First = 1;
			} else if (Object != nullptr && Object->PropertyClass->IsChildOf(UWidget::StaticClass()) && SeesWidget(Blueprint, Steps[0].Name)) {
				Out.SetWidgetName(Steps[0].Name);
				On = Object->PropertyClass;
				First = 1;
			}
			/* A widget only a parent blueprint's tree holds, while this one has a tree of its own (the content of the
			 * parent's named slots). 5.8's view compiler looks a widget up in the blueprint's own tree, and in the
			 * parent's only when there is none (CreateWidgetMap), so it would not find it; the game's compiler did.
			 * Read from self it is the same field path (the inherited widget property, then the field), which is what
			 * the view class was cooked with. */
		}

		if (First == 0) Out.SetSelfContext();

		for (int32 At = First; At < Steps.Num(); ++At) {
			const FStep& Step = Steps[At];
			UE::MVVM::FMVVMConstFieldVariant Field;

			if (Step.Owner.IsValid()) Field = FindField(ResolveOwner(Step.Owner, Blueprint), Step.bProperty, Step.Name);
			if (Field.IsEmpty()) Field = FindField(On, Step.bProperty, Step.Name);

			if (Field.IsEmpty()) {
				Missing = Step.Name.ToString();

				return false;
			}

			Out.AppendPropertyPath(Blueprint, Field);
			On = TypeOf(Field);
		}

		return true;
	};

	auto ToSteps = [](const TArray<FField>& Fields) {
		TArray<FStep> Steps;

		for (const FField& Field : Fields) {
			FStep Step;
			Step.bProperty = Field.bProperty;
			Step.Owner = Field.Owner;
			Step.Name = Field.Name;
			Steps.Add(Step);
		}

		return Steps;
	};

	/* The source that is the widget itself, where the view has one */
	int32 SelfSource = INDEX_NONE;

	{
		const TArray<TSharedPtr<FJsonValue>> Sources = View->GetArrayField(TEXT("Sources"));

		for (int32 At = 0; At < Sources.Num(); ++At) {
			if ((static_cast<int32>(Sources[At]->AsObject()->GetNumberField(TEXT("Flags"))) & SelfReference) != 0) SelfSource = At;
		}
	}

	const UEnum* Modes = StaticEnum<EMVVMExecutionMode>();
	int32 Added = 0;
	int32 Index = -1;

	for (const TSharedPtr<FJsonValue>& Value : View->GetArrayField(TEXT("Bindings"))) {
		Index++;

		const TSharedPtr<FJsonObject> Cooked = Value->AsObject();
		const TSharedPtr<FJsonObject> Compiled = Cooked->GetObjectField(TEXT("Binding"));
		const int32 Flags = static_cast<int32>(Cooked->GetNumberField(TEXT("Flags")));
		const EBindingType Type = static_cast<EBindingType>(Compiled->GetNumberField(TEXT("Type")));

		const TArray<FField> SourceFields = Path(Compiled->GetObjectField(TEXT("SourceFieldPath")));
		const TArray<FField> DestinationFields = Path(Compiled->GetObjectField(TEXT("DestinationFieldPath")));
		const TArray<FField> ConversionFields = Path(Compiled->GetObjectField(TEXT("ConversionFunctionFieldPath")));

		auto Skip = [this, Blueprint, Index](const FString& Why) {
			Say(EImportIssue::Data,
				FString::Printf(TEXT("Binding %d of %s was left out"), Index, *Blueprint->GetName()),
				Why);
		};

		if (Type == EBindingType::BindingComplex) {
			Skip(TEXT("It was compiled as a complex binding, which writes its destination from a function of its own and is not read back yet."));
			continue;
		}

		FMVVMBlueprintPropertyPath Destination;
		FString Missing;
		TArray<FStep> DestinationSteps = ToSteps(DestinationFields);
		bool bWritesViewModel = false;

		/* A viewmodel set from a binding. The editor's destination is the viewmodel's own property, which the compile
		 * writes through the setter it makes for it, and only a write like that makes the widget one of the binding's
		 * sources; a binding that calls a function of the widget's does not. */
		if (DestinationSteps.Num() == 1 && !DestinationSteps[0].bProperty && SelfSource != INDEX_NONE
			&& (static_cast<uint64>(Cooked->GetNumberField(TEXT("SourceBitField"))) & (1ull << SelfSource)) != 0) {
			const FString Setter = DestinationSteps[0].Name.ToString();

			for (const TPair<FName, FGuid>& ViewModel : ViewModels) {
				if (Setter == TEXT("Set") + ViewModel.Key.ToString()) {
					DestinationSteps[0].bProperty = true;
					DestinationSteps[0].Owner.Reset();
					DestinationSteps[0].Name = ViewModel.Key;
					bWritesViewModel = true;
				}
			}
		}

		if (!MakePath(DestinationSteps, Destination, Missing, bWritesViewModel)) {
			Skip(FString::Printf(TEXT("Its destination reaches %s, which the class doesn't have."), *Missing));
			continue;
		}

		/* The conversion, and what it is handed */
		const UFunction* Converts = nullptr;
		TArray<TPair<FName, TSharedPtr<FJsonObject>>> Arguments;
		FConversionCall Read;
		FGuid BindingId;

		if (Type == EBindingType::HasConversionFunction && ConversionFields.Num() > 0) {
			const FField& Field = ConversionFields.Last();
			Converts = FindField(ResolveOwner(Field.Owner, Blueprint), false, Field.Name).GetFunction();
		} else if (Type == EBindingType::HasComplexConversionFunction && ConversionFields.Num() > 0) {
			/* Named after the binding it was made for: __<binding>_SourceToDest */
			const FString Wrapper = ConversionFields.Last().Name.ToString();
			FString Id = Wrapper;
			Id.RemoveFromStart(TEXT("__"));
			Id.RemoveFromEnd(TEXT("_SourceToDest"));
			FGuid::Parse(Id, BindingId);

			const FUObjectExport* Function = nullptr;

			for (const FUObjectExport* Export : Container->Exports) {
				if (Export != nullptr && Export->IsJsonValid() && Export->GetType() == TEXT("Function") && Export->GetName().ToString() == Wrapper) Function = Export;
			}

			if (Function == nullptr || !ReadConversion(Function->JsonObject, Read)) {
				Skip(FString::Printf(TEXT("Its conversion function %s couldn't be read back."), *Wrapper));
				continue;
			}

			if (const TSharedPtr<FJsonObject>* Named; Read.Call->TryGetObjectField(TEXT("Function"), Named)) {
				FString Owner, Name;
				(*Named)->GetStringField(TEXT("ObjectName")).Split(TEXT(":"), &Owner, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

				TSharedPtr<FJsonObject> OwnerReference = MakeShared<FJsonObject>();
				OwnerReference->SetStringField(TEXT("ObjectName"), Owner + TEXT("'"));
				OwnerReference->SetStringField(TEXT("ObjectPath"), (*Named)->GetStringField(TEXT("ObjectPath")));

				Converts = FindField(ResolveOwner(OwnerReference, Blueprint), false, FName(*Name.LeftChop(1))).GetFunction();
			} else {
				Converts = FindField(SelfClass(), false, FName(*Read.Call->GetStringField(TEXT("Function")))).GetFunction();
			}
		}

		if (Type != EBindingType::None && Converts == nullptr) {
			Skip(TEXT("Its conversion function isn't on the class it was called on."));
			continue;
		}

		FMVVMBlueprintViewBinding& Binding = BlueprintView->AddDefaultBinding();

		if (BindingId.IsValid()) Binding.BindingId = BindingId;

		Binding.DestinationPath = Destination;
		Binding.BindingType = (Flags & OneWay) != 0 ? EMVVMBindingMode::OneWayToDestination : EMVVMBindingMode::OneTimeToDestination;
		Binding.bEnabled = (Flags & EnabledByDefault) != 0;
		Binding.bOverrideExecutionMode = (Flags & OverrideExecuteMode) != 0;

		if (Binding.bOverrideExecutionMode) {
			const int64 Mode = Modes->GetValueByNameString(Cooked->GetStringField(TEXT("ExecutionMode")));

			if (Mode != INDEX_NONE) Binding.OverrideExecutionMode = static_cast<EMVVMExecutionMode>(Mode);
		}

		if (Type == EBindingType::None) {
			if (!MakePath(ToSteps(SourceFields), Binding.SourcePath, Missing)) {
				Skip(FString::Printf(TEXT("Its source reaches %s, which the class doesn't have."), *Missing));
				BlueprintView->RemoveBinding(&Binding);
				continue;
			}

			Added++;
			continue;
		}

		/* The inputs it takes, by where they sit among its parameters, read before the conversion is made */
		struct FInput {
			FName Name;
			int32 Position = 0;
		};

		TArray<FInput> Inputs;
		const FString ConvertsName = Converts->GetName();

		{
			int32 Position = 0;

			for (TFieldIterator<FProperty> It(Converts); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) {
				if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

				/* Something it hands back rather than takes */
				if (!It->HasAnyPropertyFlags(CPF_OutParm) || It->HasAnyPropertyFlags(CPF_ReferenceParm)) Inputs.Add({ It->GetFName(), Position });

				Position++;
			}
		}

		/* What the editor does when a conversion is picked, short of asking whether it would have offered it */
		UMVVMBlueprintViewConversionFunction* Conversion = NewObject<UMVVMBlueprintViewConversionFunction>(Blueprint);
		Binding.Conversion.SourceToDestinationConversion = Conversion;
		Conversion->SetDestinationPath(Binding.DestinationPath);
		Conversion->Initialize(Blueprint, UE::MVVM::ConversionFunctionHelper::CreateWrapperName(Binding, true), FMVVMBlueprintFunctionReference(Blueprint, Converts));

		/* Each input the function takes, against what the binding hands it */
		TArray<TSharedPtr<FJsonValue>> Parameters;

		if (Read.Call.IsValid()) Parameters = Read.Call->GetArrayField(TEXT("Parameters"));

		bool bSimpleSourceSet = false;
		TArray<FString> Unread;

		for (const FInput& Input : Inputs) {
			const int32 Position = Input.Position;
			const FString InputName = Input.Name.ToString();
			const FMVVMBlueprintPinId Pin(TArray<FName>{ Input.Name });

			if (Type == EBindingType::HasConversionFunction) {
				/* A simple one takes the binding's source as its one input */
				if (bSimpleSourceSet) continue;

				FMVVMBlueprintPropertyPath Source;

				if (!MakePath(ToSteps(SourceFields), Source, Missing)) {
					Unread.Add(FString::Printf(TEXT("%s (reaches %s)"), *InputName, *Missing));
					continue;
				}

				Conversion->SetGraphPin(Blueprint, Pin, Source);
				bSimpleSourceSet = true;

				continue;
			}

			if (!Parameters.IsValidIndex(Position)) {
				Unread.Add(InputName);
				continue;
			}

			const TSharedPtr<FJsonObject> Argument = Parameters[Position]->AsObject();

			if (TArray<FStep> Steps; ReadPath(Argument, Read.Locals, Steps)) {
				FMVVMBlueprintPropertyPath Source;

				if (!MakePath(Steps, Source, Missing)) {
					Unread.Add(FString::Printf(TEXT("%s (reaches %s)"), *InputName, *Missing));
					continue;
				}

				Conversion->SetGraphPin(Blueprint, Pin, Source);

				UE_LOG(LogReflection, Display, TEXT("%s: %s takes %s <- %s"), *Blueprint->GetName(), *ConvertsName, *InputName, *Source.ToString(Blueprint, true, true));
			} else if (UEdGraphPin* GraphPin = Conversion->GetOrCreateGraphPin(Blueprint, Pin)) {
				if (!SetDefault(GraphPin, Argument)) Unread.Add(FString::Printf(TEXT("%s (%s)"), *InputName, *Argument->GetStringField(TEXT("Token"))));
			}
		}

		Conversion->SavePinValues(Blueprint);

		if (Unread.Num() > 0) {
			Say(EImportIssue::Data,
				FString::Printf(TEXT("Binding %d of %s lost some of what its conversion is handed"), Index, *Blueprint->GetName()),
				FString::Printf(TEXT("%s takes %s, and those inputs were left at their defaults."), *ConvertsName, *FString::Join(Unread, TEXT(", "))));
		}

		Added++;
	}

	if (Added > 0) {
		UE_LOG(LogReflection, Display, TEXT("%s: %d viewmodel(s) and %d binding(s) put back in its view"), *Blueprint->GetName(), ViewModels.Num(), Added);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return Added;
}

#else

int32 FWidgetViewModels::ConstructViewModels(UWidgetBlueprint*, UObjectSerializer*) { return 0; }
int32 FWidgetViewModels::ConstructBindings(UWidgetBlueprint*) { return 0; }

#endif
