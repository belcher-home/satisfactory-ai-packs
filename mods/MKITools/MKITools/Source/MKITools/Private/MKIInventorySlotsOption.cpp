#include "MKIInventorySlotsOption.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "MKIPlayerSubsystem.h"

#define LOCTEXT_NAMESPACE "MKITools"

UUserWidget* UMKIInventorySlotsOption::CreateEditorWidget_Implementation(UUserWidget* ParentWidget) const
{
	return ParentWidget ? CreateWidget<UMKIInventorySlotsWidget>(ParentWidget, UMKIInventorySlotsWidget::StaticClass()) : nullptr;
}

TSharedRef<SWidget> UMKIInventorySlotsWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
		Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Label->SetText(LOCTEXT("InventorySlotsLoading", "Inventory slots"));
		if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(Label))
		{
			LabelSlot->SetVerticalAlignment(VAlign_Center);
			LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			LabelSlot->SetPadding(FMargin(0.f, 4.f, 16.f, 4.f));
		}
		AddButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("AddButton"));
		UTextBlock* ButtonText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ButtonText"));
		ButtonText->SetText(FText::Format(LOCTEXT("InventorySlotsAdd", "+{0} slots"), AMKIPlayerSubsystem::InventorySlotsPerPress));
		ButtonText->SetColorAndOpacity(FSlateColor(FLinearColor::Black));
		AddButton->AddChild(ButtonText);
		AddButton->OnClicked.AddDynamic(this, &UMKIInventorySlotsWidget::OnAddClicked);
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(AddButton)) ButtonSlot->SetVerticalAlignment(VAlign_Center);
		WidgetTree->RootWidget = Row;
	}
	return Super::RebuildWidget();
}

void UMKIInventorySlotsWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!Label || !AddButton) return;
	const AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(GetOwningPlayerPawn());
	const UFGInventoryComponent* Inventory = Character ? Character->GetInventory() : nullptr;
	const int32 Extra = AMKIPlayerSubsystem::GetExtraInventorySlots(this);
	if (!Inventory)
	{
		Label->SetText(LOCTEXT("InventorySlotsNoGame", "Inventory slots: load a save to add slots"));
		AddButton->SetIsEnabled(false);
		return;
	}
	Label->SetText(FText::Format(LOCTEXT("InventorySlots", "Inventory slots: {0} ({1} of {2} extra added; can only grow)"), Inventory->GetSizeLinear(), Extra, AMKIPlayerSubsystem::MaximumExtraInventorySlots));
	AddButton->SetIsEnabled(AMKIPlayerSubsystem::CanAddInventorySlots(this));
}

void UMKIInventorySlotsWidget::OnAddClicked()
{
	AMKIPlayerSubsystem::AddInventorySlots(this);
}

#undef LOCTEXT_NAMESPACE
