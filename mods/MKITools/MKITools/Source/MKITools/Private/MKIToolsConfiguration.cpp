#include "MKIToolsConfiguration.h"

#include "Configuration/ConfigManager.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertyFloat.h"
#include "Configuration/Properties/ConfigPropertyInteger.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "MKIInventorySlotsOption.h"
#include "MKITools.h"

#define LOCTEXT_NAMESPACE "MKITools"

namespace
{
	/**
	 * SML draws an option only through its Blueprint property classes (their CreateEditorWidget); the native classes draw
	 * nothing. Live (2026-10-20): with native properties the MKI Tools page showed no option at all.
	 */
	const TCHAR* const SectionWidgetClass = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertySection.BP_ConfigPropertySection_C");
	/** Whole-number sliders (the maintainer, 2026-10-20: a float slider could not land on 5.0). */
	const TCHAR* const IntegerWidgetClass = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyInteger.BP_ConfigPropertyInteger_C");
	const TCHAR* const FloatWidgetClass = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyFloat.BP_ConfigPropertyFloat_C");
	const TCHAR* const BoolWidgetClass = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyBool.BP_ConfigPropertyBool_C");

	template <typename T>
	T* NewProperty(UObject* Outer, const TCHAR* BlueprintClass, FName Name)
	{
		UClass* Class = LoadClass<T>(nullptr, BlueprintClass);
		if (!Class)
		{
			UE_LOG(LogMKITools, Warning, TEXT("Options: SML's %s is missing; the option is saved but not shown"), BlueprintClass);
			Class = T::StaticClass();
		}
		return NewObject<T>(Outer, Class, Name, RF_Public | RF_ArchetypeObject);
	}

	/**
	 * Sets a Blueprint variable of SML's float property (MinValue, MaxValue: float or double), logging what it was.
	 * Live (2026-10-20): left unset, its range clamped every typed value back to 1.0.
	 */
	void SetBlueprintNumber(UObject* Object, const TCHAR* Name, double Value)
	{
		FProperty* Property = Object->GetClass()->FindPropertyByName(Name);
		if (FDoubleProperty* Double = CastField<FDoubleProperty>(Property))
		{
			UE_LOG(LogMKITools, Log, TEXT("Options: %s.%s was %g, set to %g"), *Object->GetName(), Name, Double->GetPropertyValue_InContainer(Object), Value);
			Double->SetPropertyValue_InContainer(Object, Value);
		}
		else if (FFloatProperty* Float = CastField<FFloatProperty>(Property))
		{
			UE_LOG(LogMKITools, Log, TEXT("Options: %s.%s was %g, set to %g"), *Object->GetName(), Name, Float->GetPropertyValue_InContainer(Object), Value);
			Float->SetPropertyValue_InContainer(Object, static_cast<float>(Value));
		}
		else if (FIntProperty* Int = CastField<FIntProperty>(Property))
		{
			UE_LOG(LogMKITools, Log, TEXT("Options: %s.%s was %d, set to %g"), *Object->GetName(), Name, Int->GetPropertyValue_InContainer(Object), Value);
			Int->SetPropertyValue_InContainer(Object, FMath::RoundToInt(Value));
		}
		else UE_LOG(LogMKITools, Warning, TEXT("Options: %s has no number %s"), *Object->GetName(), Name);
	}

	/** Sets a Blueprint enum variable to the entry whose name contains `EntryPart` (the float widget's type: the slider). */
	void SetBlueprintEnum(UObject* Object, const TCHAR* EntryPart)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const UEnum* Enum = nullptr; FNumericProperty* Underlying = nullptr;
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(*It)) { Enum = EnumProperty->GetEnum(); Underlying = EnumProperty->GetUnderlyingProperty(); }
			else if (FByteProperty* ByteProperty = CastField<FByteProperty>(*It)) { Enum = ByteProperty->Enum; Underlying = ByteProperty; }
			if (!Enum || !Underlying || !Enum->GetName().Contains(TEXT("WidgetType"))) continue;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (!Enum->GetNameStringByIndex(Index).Contains(EntryPart)) continue;
				Underlying->SetIntPropertyValue(It->ContainerPtrToValuePtr<void>(Object), Enum->GetValueByIndex(Index));
				UE_LOG(LogMKITools, Log, TEXT("Options: %s.%s set to %s"), *Object->GetName(), *It->GetName(), *Enum->GetNameStringByIndex(Index));
				return;
			}
		}
		UE_LOG(LogMKITools, Warning, TEXT("Options: %s has no widget type %s"), *Object->GetName(), EntryPart);
	}
}

UMKIToolsConfiguration::UMKIToolsConfiguration()
{
	ConfigId.ModReference = TEXT("MKITools");
	DisplayName = LOCTEXT("ConfigurationName", "MKI Tools");
	Description = LOCTEXT("ConfigurationDescription", "Options for MKI Tools.");
}

void UMKIToolsConfiguration::BuildOptions()
{
	UMKIToolsConfiguration* Defaults = GetMutableDefault<UMKIToolsConfiguration>();
	if (Defaults->RootSection) return;
	UConfigPropertySection* Root = NewProperty<UConfigPropertySection>(Defaults, SectionWidgetClass, TEXT("RootSection"));
	// The maintainer (2026-10-20): the options one under another; SML's section widget defaults to a row that runs off the page.
	SetBlueprintEnum(Root, TEXT("Vertical"));
	// The maintainer (2026-10-20): a hand crafting speed for the Craft Bench, 1x to 10x, default 1x (the game's own).
	UConfigPropertyInteger* Speed = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("HandCraftingSpeed"));
	Speed->DisplayName = LOCTEXT("HandCraftingSpeed", "Hand crafting multiplier");
	Speed->Tooltip = LOCTEXT("HandCraftingSpeedTooltip", "How much crafting work each Craft Bench tick does. The ticks themselves keep the game's pace; each one counts as this many ticks, so it can finish up to this many items. 1 is the game's own, 10 is ten ticks' worth per tick. From 1 to 10.");
	Speed->DefaultValue = 1;
	Speed->Value = 1;
	// The maintainer: editable in game (it applies at the next craft step), not only from the main menu.
	Speed->bRequiresWorldReload = false;
	Speed->bHidden = false;
	// SML's float widget clamps to its own range (Blueprint variables, not the native property): 1 to 10, as a slider.
	SetBlueprintNumber(Speed, TEXT("MinValue"), MinimumHandCraftingSpeed);
	SetBlueprintNumber(Speed, TEXT("MaxValue"), MaximumHandCraftingSpeed);
	SetBlueprintEnum(Speed, TEXT("Slider"));
	Root->SectionProperties.Add(HandCraftingSpeedKey, Speed);
	// The maintainer (2026-10-21): a hand mining speed for mining resource nodes by hand, 1x to 10x, default 1x.
	UConfigPropertyInteger* Mining = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("HandMiningSpeed"));
	Mining->DisplayName = LOCTEXT("HandMiningSpeed", "Hand mining multiplier");
	Mining->Tooltip = LOCTEXT("HandMiningSpeedTooltip", "How much ore each swing gives when mining a resource node by hand. The swings keep the game's pace; each one gives this many times the ore. 1 is the game's own, 10 is ten times the ore per swing. From 1 to 10.");
	Mining->DefaultValue = 1;
	Mining->Value = 1;
	Mining->bRequiresWorldReload = false;
	Mining->bHidden = false;
	SetBlueprintNumber(Mining, TEXT("MinValue"), 1.0);
	SetBlueprintNumber(Mining, TEXT("MaxValue"), MaximumHandMiningSpeed);
	SetBlueprintEnum(Mining, TEXT("Slider"));
	Root->SectionProperties.Add(HandMiningSpeedKey, Mining);
	// The maintainer (2026-10-21): a slider for health regeneration speed, 1x to 10x, default 1x.
	UConfigPropertyInteger* Regeneration = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("HealthRegenerationSpeed"));
	Regeneration->DisplayName = LOCTEXT("HealthRegenerationSpeed", "Health regeneration speed");
	Regeneration->Tooltip = LOCTEXT("HealthRegenerationSpeedTooltip", "How fast your health regenerates on its own, as a multiple of the game's own rate. The wait after taking damage and the level regeneration stops at stay the game's own. 1 is the game's own, 10 is ten times as fast. From 1 to 10.");
	Regeneration->DefaultValue = 1;
	Regeneration->Value = 1;
	Regeneration->bRequiresWorldReload = false;
	Regeneration->bHidden = false;
	SetBlueprintNumber(Regeneration, TEXT("MinValue"), 1.0);
	SetBlueprintNumber(Regeneration, TEXT("MaxValue"), MaximumHealthRegenerationSpeed);
	SetBlueprintEnum(Regeneration, TEXT("Slider"));
	Root->SectionProperties.Add(HealthRegenerationSpeedKey, Regeneration);
	// The maintainer (2026-10-21, toned down 2026-10-22): how much faster the player runs or moves in the air, 0 to 10 steps of 10%.
	UConfigPropertyInteger* Movement = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("MovementSpeedBonus"));
	Movement->DisplayName = LOCTEXT("MovementSpeedBonus", "Movement speed bonus");
	Movement->Tooltip = LOCTEXT("MovementSpeedBonusTooltip", "How much faster you move than the game's own speed, in steps of 10%: walking, sprinting, crouching, swimming and moving in the air. 0 is the game's own, 5 is 50% faster, 10 is twice as fast. Slides, ladders, hypertubes, ziplines and the parachute keep the game's speed. From 0 to 10.");
	Movement->DefaultValue = 0;
	Movement->Value = 0;
	Movement->bRequiresWorldReload = false;
	Movement->bHidden = false;
	SetBlueprintNumber(Movement, TEXT("MinValue"), 0.0);
	SetBlueprintNumber(Movement, TEXT("MaxValue"), MaximumMovementSpeedSteps);
	SetBlueprintEnum(Movement, TEXT("Slider"));
	Root->SectionProperties.Add(MovementSpeedKey, Movement);
	// The maintainer (2026-10-21): automatically pick up nearby items, with a 1-10 radius (1 = 5 m, as the chainsaw's area).
	UConfigPropertyBool* Collect = NewProperty<UConfigPropertyBool>(Root, BoolWidgetClass, TEXT("CollectNearby"));
	Collect->DisplayName = LOCTEXT("CollectNearby", "Collect nearby items");
	Collect->Tooltip = LOCTEXT("CollectNearbyTooltip", "Automatically picks up, around you: berries, nuts and bacon agaric; items lying on the ground (such as materials around crash sites, power slugs, somersloops, mercer spheres and creature remains); and the contents of dismantle crates and death crates.");
	Collect->DefaultValue = false;
	Collect->Value = false;
	Collect->bRequiresWorldReload = false;
	Collect->bHidden = false;
	Root->SectionProperties.Add(CollectNearbyKey, Collect);
	UConfigPropertyInteger* CollectRadius = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("CollectRadiusMultiplier"));
	CollectRadius->DisplayName = LOCTEXT("CollectRadiusMultiplier", "Collect radius");
	CollectRadius->Tooltip = LOCTEXT("CollectRadiusMultiplierTooltip", "How far away \"Collect nearby items\" reaches, as a multiple of 5 m (the chainsaw's own area): 1 is 5 m, 10 is 50 m. From 1 to 10.");
	CollectRadius->DefaultValue = 1;
	CollectRadius->Value = 1;
	CollectRadius->bRequiresWorldReload = false;
	CollectRadius->bHidden = false;
	SetBlueprintNumber(CollectRadius, TEXT("MinValue"), 1.0);
	SetBlueprintNumber(CollectRadius, TEXT("MaxValue"), MaximumCollectRadiusMultiplier);
	SetBlueprintEnum(CollectRadius, TEXT("Slider"));
	Root->SectionProperties.Add(CollectRadiusMultiplierKey, CollectRadius);
	// The maintainer (2026-10-22): vanilla conveyor lifts build short by default, and extend as normal.
	UConfigPropertyBool* ShortLifts = NewProperty<UConfigPropertyBool>(Root, BoolWidgetClass, TEXT("ShortConveyorLifts"));
	ShortLifts->DisplayName = LOCTEXT("ShortConveyorLifts", "Short conveyor lifts");
	ShortLifts->Tooltip = LOCTEXT("ShortConveyorLiftsTooltip", "Conveyor lifts can be built as short as the game otherwise allows only between two connections stacked above each other, anywhere, and still extend in the game's own steps.");
	ShortLifts->DefaultValue = true;
	ShortLifts->Value = true;
	ShortLifts->bRequiresWorldReload = false;
	ShortLifts->bHidden = false;
	Root->SectionProperties.Add(ShortConveyorLiftsKey, ShortLifts);
	// The maintainer (2026-10-21): a button adding 10 inventory slots per press (up to 500), the current size shown, and the
	// slots can only grow. SML's menu has no button: this native entry draws its own (UMKIInventorySlotsOption).
	UMKIInventorySlotsOption* Slots = NewObject<UMKIInventorySlotsOption>(Root, TEXT("InventorySlots"), RF_Public | RF_ArchetypeObject);
	Slots->DisplayName = LOCTEXT("InventorySlots", "Inventory slots");
	Slots->Tooltip = LOCTEXT("InventorySlotsTooltip", "Adds 10 inventory slots per press, up to 500 added in each save. Slots can only be added, never taken away.");
	Slots->bRequiresWorldReload = false;
	Slots->bHidden = false;
	Root->SectionProperties.Add(InventorySlotsKey, Slots);
	// The maintainer (2026-10-20): the chainsaw cuts the game's rock foliage (boulders), on by default, editable in game.
	UConfigPropertyBool* Boulders = NewProperty<UConfigPropertyBool>(Root, BoolWidgetClass, TEXT("ChainsawCutsBoulders"));
	Boulders->DisplayName = LOCTEXT("ChainsawCutsBoulders", "Chainsaw cuts boulders");
	Boulders->Tooltip = LOCTEXT("ChainsawCutsBouldersTooltip", "Lets the chainsaw cut the boulders and rock clusters the game places like foliage, for limestone. Removal is permanent, like any chainsawed foliage.");
	Boulders->DefaultValue = true;
	Boulders->Value = true;
	Boulders->bRequiresWorldReload = false;
	Boulders->bHidden = false;
	Root->SectionProperties.Add(ChainsawCutsBouldersKey, Boulders);
	// The maintainer (2026-10-20): the chainsaw's clearance (area cut) radius as a multiplier of the game's own (5 m), 1x to 10x.
	UConfigPropertyInteger* Radius = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("ChainsawClearanceMultiplier"));
	Radius->DisplayName = LOCTEXT("ChainsawClearanceMultiplier", "Chainsaw clearance radius");
	Radius->Tooltip = LOCTEXT("ChainsawClearanceMultiplierTooltip", "How far around a sawn tree or bush the chainsaw also clears and picks up foliage, as a multiple of the game's own radius (5 m): 1 is the game's own, 10 is 50 m. From 1 to 10.");
	Radius->DefaultValue = 1;
	Radius->Value = 1;
	Radius->bRequiresWorldReload = false;
	Radius->bHidden = false;
	SetBlueprintNumber(Radius, TEXT("MinValue"), 1.0);
	SetBlueprintNumber(Radius, TEXT("MaxValue"), MaximumChainsawClearanceMultiplier);
	SetBlueprintEnum(Radius, TEXT("Slider"));
	Root->SectionProperties.Add(ChainsawClearanceMultiplierKey, Radius);
	// The maintainer (2026-10-20): cut vegetation regrows at random, more likely the longer it has been gone; off by default.
	UConfigPropertyBool* Regrowth = NewProperty<UConfigPropertyBool>(Root, BoolWidgetClass, TEXT("VegetationRegrowth"));
	Regrowth->DisplayName = LOCTEXT("VegetationRegrowth", "Vegetation regrowth");
	Regrowth->Tooltip = LOCTEXT("VegetationRegrowthTooltip", "Cut trees, bushes and plants grow back at random, more likely the longer they have been gone. A plant comes back only while its area is out of view (it is there the next time the area loads), never under or next to a building. Boulders never grow back.");
	Regrowth->DefaultValue = false;
	Regrowth->Value = false;
	Regrowth->bRequiresWorldReload = false;
	Regrowth->bHidden = false;
	Root->SectionProperties.Add(VegetationRegrowthKey, Regrowth);
	UConfigPropertyInteger* Hours = NewProperty<UConfigPropertyInteger>(Root, IntegerWidgetClass, TEXT("RegrowthHours"));
	Hours->DisplayName = LOCTEXT("RegrowthHours", "Regrowth time (hours of play)");
	Hours->Tooltip = LOCTEXT("RegrowthHoursTooltip", "The average time a cut plant takes to grow back, in hours of play. Some come back sooner, some later; the longer one has been gone, the more likely it returns. From 1 to 10.");
	Hours->DefaultValue = DefaultRegrowthHours;
	Hours->Value = DefaultRegrowthHours;
	Hours->bRequiresWorldReload = false;
	Hours->bHidden = false;
	SetBlueprintNumber(Hours, TEXT("MinValue"), 1.0);
	SetBlueprintNumber(Hours, TEXT("MaxValue"), MaximumRegrowthHours);
	SetBlueprintEnum(Hours, TEXT("Slider"));
	Root->SectionProperties.Add(RegrowthHoursKey, Hours);
	Defaults->RootSection = Root;
}

void UMKIToolsConfiguration::AdoptOptions(UConfigManager* Config)
{
	// Live (2026-10-20): SML's live copy of the options kept referring to the very objects built on the class defaults,
	// whose outers end at the class defaults, so the menu's widget found no world ("A null object was passed as a world
	// context object"), refused every edit (a typed 5 went back to 1.0) and treated the option as main-menu only. Each
	// option is moved into the live root section (keeping the value loaded from disk), so its outers reach SML's config
	// holder, its game instance and world, and marking it dirty saves it.
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	UConfigPropertySection* Live = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	if (!Live) return;
	for (TPair<FString, TObjectPtr<UConfigProperty>>& Entry : Live->SectionProperties)
	{
		UConfigProperty* Property = Entry.Value;
		if (!Property || Property->GetOuter() == Live) continue;
		Entry.Value = DuplicateObject<UConfigProperty>(Property, Live, Property->GetFName());
		Entry.Value->ClearFlags(RF_ArchetypeObject | RF_Public);
	}
}

float UMKIToolsConfiguration::GetHandMiningSpeed(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(HandMiningSpeedKey) : nullptr;
	const UConfigPropertyInteger* Mining = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	return Mining ? FMath::Clamp(static_cast<float>(Mining->Value), 1.f, MaximumHandMiningSpeed) : 1.f;
}

float UMKIToolsConfiguration::GetHealthRegenerationSpeed(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(HealthRegenerationSpeedKey) : nullptr;
	const UConfigPropertyInteger* Regeneration = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	return Regeneration ? FMath::Clamp(static_cast<float>(Regeneration->Value), 1.f, MaximumHealthRegenerationSpeed) : 1.f;
}

float UMKIToolsConfiguration::GetMovementSpeed(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(MovementSpeedKey) : nullptr;
	const UConfigPropertyInteger* Movement = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	// The maintainer (2026-10-22): 2x was still very fast and 10x ludicrous; each step is 10%, so 10 is twice the speed.
	return Movement ? 1.f + MovementSpeedStep * FMath::Clamp(Movement->Value, 0, static_cast<int32>(MaximumMovementSpeedSteps)) : 1.f;
}

bool UMKIToolsConfiguration::GetCollectNearby(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(CollectNearbyKey) : nullptr;
	const UConfigPropertyBool* Collect = Property ? Cast<UConfigPropertyBool>(Property->Get()) : nullptr;
	return Collect ? Collect->Value : false;
}

float UMKIToolsConfiguration::GetCollectRadiusMultiplier(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(CollectRadiusMultiplierKey) : nullptr;
	const UConfigPropertyInteger* Radius = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	return Radius ? FMath::Clamp(static_cast<float>(Radius->Value), 1.f, MaximumCollectRadiusMultiplier) : 1.f;
}

bool UMKIToolsConfiguration::GetShortConveyorLifts(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(ShortConveyorLiftsKey) : nullptr;
	const UConfigPropertyBool* ShortLifts = Property ? Cast<UConfigPropertyBool>(Property->Get()) : nullptr;
	return ShortLifts ? ShortLifts->Value : true;
}

float UMKIToolsConfiguration::GetHandCraftingSpeed(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(HandCraftingSpeedKey) : nullptr;
	const UConfigPropertyInteger* Speed = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	// A value typed outside the range (the option is a plain number) is held to it.
	return Speed ? FMath::Clamp(static_cast<float>(Speed->Value), MinimumHandCraftingSpeed, MaximumHandCraftingSpeed) : MinimumHandCraftingSpeed;
}



bool UMKIToolsConfiguration::GetChainsawCutsBoulders(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(ChainsawCutsBouldersKey) : nullptr;
	const UConfigPropertyBool* Boulders = Property ? Cast<UConfigPropertyBool>(Property->Get()) : nullptr;
	return Boulders ? Boulders->Value : true;
}


float UMKIToolsConfiguration::GetChainsawClearanceMultiplier(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(ChainsawClearanceMultiplierKey) : nullptr;
	const UConfigPropertyInteger* Radius = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	return Radius ? FMath::Clamp(static_cast<float>(Radius->Value), 1.f, MaximumChainsawClearanceMultiplier) : 1.f;
}

bool UMKIToolsConfiguration::GetVegetationRegrowth(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(VegetationRegrowthKey) : nullptr;
	const UConfigPropertyBool* Regrowth = Property ? Cast<UConfigPropertyBool>(Property->Get()) : nullptr;
	return Regrowth ? Regrowth->Value : false;
}

float UMKIToolsConfiguration::GetRegrowthHours(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UConfigManager* Config = GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	FConfigId Id; Id.ModReference = TEXT("MKITools");
	const UConfigPropertySection* Root = Config ? Config->GetConfigurationRootSection(Id) : nullptr;
	const TObjectPtr<UConfigProperty>* Property = Root ? Root->SectionProperties.Find(RegrowthHoursKey) : nullptr;
	const UConfigPropertyInteger* Hours = Property ? Cast<UConfigPropertyInteger>(Property->Get()) : nullptr;
	return Hours ? FMath::Clamp(static_cast<float>(Hours->Value), 1.f, MaximumRegrowthHours) : DefaultRegrowthHours;
}

#undef LOCTEXT_NAMESPACE
