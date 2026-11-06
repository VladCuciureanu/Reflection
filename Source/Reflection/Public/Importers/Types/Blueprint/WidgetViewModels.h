/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWidgetBlueprint;
class UObjectSerializer;
struct FUObjectExportContainer;

/* Puts back a widget blueprint's viewmodels and the bindings drawn between them and its widgets.
 *
 * In the editor these live on the blueprint, in its MVVM extension: a list of viewmodels, each with
 * a class, a name and the way it is got hold of, and a list of bindings, each a source path, a
 * destination path and maybe a conversion function between them. None of that is cooked. What is
 * cooked is what compiling it made: an MVVMViewClass on the generated class, holding the sources
 * the view reads, the bindings as indices into a library of fields, and the functions the compiler
 * wrote for the conversions that needed one.
 *
 * Everything the editor kept is still in there, spelled the way the compiler spells it, so it is
 * read back out of that: a source made from a viewmodel is that viewmodel, its flags say how it was
 * got hold of, a binding's fields are its paths, and a conversion that needed a function of its own
 * is read back out of that function's bytecode as the call it makes and what goes into each pin.
 * The extension is rebuilt from it, and compiling the blueprint makes the view again.
 *
 * Some of what the compile makes was also cooked as ordinary members of the class: a property for
 * each viewmodel, a setter for the ones that can be set, and the conversion functions. Those are the
 * compile's to make, so the rest of the import leaves them out (GetProperties, GetFunctions). */
struct REFLECTION_API FWidgetViewModels {
	/* Reads the view cooked onto a widget blueprint's generated class, where it has one */
	FWidgetViewModels(FUObjectExportContainer* Container, const TSharedPtr<FJsonObject>& ClassExport);

	/* Whether there is a view to put back, and this build can put one back */
	bool IsValid() const;

	/* The properties the view's compile makes, one per viewmodel */
	const TSet<FString>& GetProperties() const { return Properties; }

	/* The functions the view's compile makes: the setters and the conversions that need a function */
	const TSet<FString>& GetFunctions() const { return Functions; }

	/* Adds the viewmodels to the blueprint's view, which makes the extension where it has none.
	 *
	 * Done before the class's defaults are read and before the graphs are laid out: a viewmodel is
	 * a property of the class once the blueprint compiles, the defaults may hold an instance of one,
	 * and the graphs read them and call their setters. Answers how many were added. */
	int32 ConstructViewModels(UWidgetBlueprint* Blueprint, UObjectSerializer* Serializer);

	/* Adds the bindings, once the functions they name are on the class. Answers how many were added */
	int32 ConstructBindings(UWidgetBlueprint* Blueprint);

private:
	FUObjectExportContainer* Container = nullptr;

	/* The MVVMViewClass's properties */
	TSharedPtr<FJsonObject> View;

	TSet<FString> Properties;
	TSet<FString> Functions;

	/* One step of a compiled field path: a property or a function, and the class or struct it is on */
	struct FField {
		bool bProperty = true;
		TSharedPtr<FJsonObject> Owner;
		FName Name;
	};

	/* The binding library's fields, in the order it loads them: every class's properties, then its functions */
	TArray<FField> LoadedProperties;
	TArray<FField> LoadedFunctions;
	TArray<TSharedPtr<FJsonValue>> FieldPaths;

	/* A compiled field path ({StartIndex, Num} into the library) as the fields it walks */
	TArray<FField> Path(const TSharedPtr<FJsonObject>& CompiledFieldPath) const;

	/* The viewmodels added, by name */
	TMap<FName, FGuid> ViewModels;
};
