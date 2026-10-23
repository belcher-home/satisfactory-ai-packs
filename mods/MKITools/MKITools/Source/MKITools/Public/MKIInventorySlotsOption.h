#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "MKIInventorySlotsOption.generated.h"

/**
 * The "Inventory slots" entry under Mods > MKI Tools (the maintainer 2026-10-21): the current inventory size and a "+10
 * slots" button, up to AMKIPlayerSubsystem::MaximumExtraInventorySlots added per save. SML's menu has no button, so this
 * native property draws its own widget; it keeps no value of its own (the slots are saved with the game).
 */
UCLASS()
class MKITOOLS_API UMKIInventorySlotsOption : public UConfigPropertyBool
{
	GENERATED_BODY()

public:
	virtual UUserWidget* CreateEditorWidget_Implementation(UUserWidget* ParentWidget) const override;
	virtual bool ResetToDefault_Implementation() override { return false; }
};

/** The entry's widget, built in code: a label with the inventory size, and the "+10 slots" button. */
UCLASS()
class MKITOOLS_API UMKIInventorySlotsWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UFUNCTION()
	void OnAddClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> Label;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> AddButton;
};
