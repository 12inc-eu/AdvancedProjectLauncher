// Copyright 12inc.eu Game Studio. All Rights Reserved.

#include "SAdvancedProjectLauncher.h"
#include "AdvancedLauncherQueue.h"

#include "ILauncherServicesModule.h"
#include "ILauncherProfileManager.h"
#include "ILauncherProfile.h"
#include "Modules/ModuleManager.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/ISlateEditableTextWidget.h" // ETextLocation
#include "Framework/Docking/TabManager.h"           // FGlobalTabmanager (open built-in launcher)
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"           // GetOSWindowHandle (portable dialog parent)
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Algo/Reverse.h"
#include "HAL/PlatformMisc.h"     // LexToString(EBuildConfiguration)
#include "HAL/PlatformProcess.h"  // FPlatformProcess::ExploreFolder
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "DesktopPlatformModule.h" // file/folder pickers (post-build file, export/import)
#include "IDesktopPlatform.h"
#include "Serialization/JsonWriter.h" // serialize a profile to a JSON file on export
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "SAdvancedProjectLauncher"

namespace AdvancedProjectLauncherUI
{
	FText GetStateText(EQueueItemState State)
	{
		switch (State)
		{
		case EQueueItemState::Pending:   return LOCTEXT("StatePending", "Pending");
		case EQueueItemState::Building:  return LOCTEXT("StateBuilding", "Building...");
		case EQueueItemState::Succeeded: return LOCTEXT("StateSucceeded", "Succeeded");
		case EQueueItemState::Failed:    return LOCTEXT("StateFailed", "Failed");
		case EQueueItemState::Canceled:  return LOCTEXT("StateCanceled", "Canceled");
		default:                         return FText::GetEmpty();
		}
	}

	FSlateColor GetStateColor(EQueueItemState State)
	{
		switch (State)
		{
		case EQueueItemState::Pending:   return FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f));
		case EQueueItemState::Building:  return FSlateColor(FLinearColor(0.95f, 0.8f, 0.2f));
		case EQueueItemState::Succeeded: return FSlateColor(FLinearColor(0.3f, 0.85f, 0.3f));
		case EQueueItemState::Failed:    return FSlateColor(FLinearColor(0.9f, 0.3f, 0.3f));
		case EQueueItemState::Canceled:  return FSlateColor(FLinearColor(0.9f, 0.6f, 0.2f));
		default:                         return FSlateColor::UseForeground();
		}
	}

	FString FormatDuration(double Seconds)
	{
		const int32 Total = FMath::Max(0, FMath::RoundToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60);
	}

	FString FormatClock(double Seconds)
	{
		const int32 Total = FMath::Max(0, FMath::RoundToInt(Seconds));
		const int32 Hours = Total / 3600;
		const int32 Minutes = (Total % 3600) / 60;
		const int32 Secs = Total % 60;
		if (Hours > 0)
		{
			return FString::Printf(TEXT("%d:%02d:%02d"), Hours, Minutes, Secs);
		}
		return FString::Printf(TEXT("%d:%02d"), Minutes, Secs);
	}
}

void SAdvancedProjectLauncher::Construct(const FArguments& InArgs)
{
	Queue = InArgs._Queue;

	if (Queue.IsValid())
	{
		QueueChangedHandle = Queue->OnChanged().AddSP(this, &SAdvancedProjectLauncher::HandleQueueChanged);
	}

	RefreshProfiles();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Header: list management
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.ToolTipText(LOCTEXT("RefreshTip", "Re-read the list of launch profiles."))
					.OnClicked(this, &SAdvancedProjectLauncher::OnRefreshClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Export", "Export..."))
					.ToolTipText(LOCTEXT("ExportTip", "Export each checked profile as its own JSON file into a folder you choose. Disabled until at least one profile is checked."))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsExportEnabled)
					.OnClicked(this, &SAdvancedProjectLauncher::OnExportClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Import", "Import..."))
					.ToolTipText(LOCTEXT("ImportTip", "Pick one or more .json / .ulp2 profile files to import and register in the current engine version."))
					.OnClicked(this, &SAdvancedProjectLauncher::OnImportClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("SelectAll", "Select All"))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsConfigEnabled)
					.OnClicked_Lambda([this]() { SelectAll(true); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("SelectNone", "Select None"))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsConfigEnabled)
					.OnClicked_Lambda([this]() { SelectAll(false); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("MoveUp", "Move Up"))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsConfigEnabled)
					.OnClicked(this, &SAdvancedProjectLauncher::OnMoveSelected, -1)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("MoveDown", "Move Down"))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsConfigEnabled)
					.OnClicked(this, &SAdvancedProjectLauncher::OnMoveSelected, 1)
				]

				// Spacer pushes the next buttons to the right edge.
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]

				// Right-aligned: open the build output folder, then the built-in Project Launcher.
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenBuild", "Open Build Folder"))
					.ToolTipText(LOCTEXT("OpenBuildTip", "Open the packaging output folder of the first selected profile (or the project folder)."))
					.OnClicked(this, &SAdvancedProjectLauncher::OnOpenBuildFolderClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Right)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenProjectLauncher", "Open Project Launcher"))
					.ToolTipText(LOCTEXT("OpenProjectLauncherTip", "Open the built-in Project Launcher tab."))
					.OnClicked(this, &SAdvancedProjectLauncher::OnOpenProjectLauncherClicked)
				]
			]

			// Profile list
			+ SVerticalBox::Slot().FillHeight(0.40f).Padding(0, 0, 0, 6)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
				.Padding(2.0f)
				[
					SAssignNew(ProfileListView, SListView<FAdvancedLauncherProfileItemPtr>)
					.ListItemsSource(&ProfileItems)
					.SelectionMode(ESelectionMode::Multi)
					.OnGenerateRow(this, &SAdvancedProjectLauncher::OnGenerateProfileRow)
				]
			]

			// Run controls
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				// Stop on first failure: moved to the left and fills the row so the action buttons sit on the right.
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).HAlign(HAlign_Left)
				[
					SNew(SCheckBox)
					.IsChecked(this, &SAdvancedProjectLauncher::GetStopOnFailureState)
					.IsEnabled(this, &SAdvancedProjectLauncher::IsConfigEnabled)
					.OnCheckStateChanged(this, &SAdvancedProjectLauncher::OnStopOnFailureChanged)
					[
						SNew(STextBlock).Text(LOCTEXT("StopOnFail", "Stop on first failure"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.ContentPadding(FMargin(24.0f, 10.0f))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsCancelEnabled)
					.OnClicked(this, &SAdvancedProjectLauncher::OnCancelClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Cancel", "Cancel"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]
				]
				// Start Build Queue: primary action, green and larger, right-aligned.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonColorAndOpacity(FLinearColor(0.10f, 0.55f, 0.15f))
					.ContentPadding(FMargin(24.0f, 10.0f))
					.ToolTipText(LOCTEXT("StartTip", "Build the checked profiles one after another, top to bottom."))
					.IsEnabled(this, &SAdvancedProjectLauncher::IsStartEnabled)
					.OnClicked(this, &SAdvancedProjectLauncher::OnStartClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("StartQueue", "Start Build Queue"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]
				]
			]

			// Post-build file: launched once when the queue finishes (not on cancel).
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("RunOnFinish", "Run on finish:"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(PostBuildBox, SEditableTextBox)
					.Text(FText::FromString(Queue.IsValid() ? Queue->GetPostBuildCommand() : FString()))
					.HintText(LOCTEXT("RunOnFinishHint", "Optional .bat, .cmd or .exe to run when the queue finishes"))
					.ToolTipText(LOCTEXT("RunOnFinishTip", "Launched once after the last profile finishes (not run if you cancel). Leave empty to disable."))
					.OnTextCommitted(this, &SAdvancedProjectLauncher::OnPostBuildTextCommitted)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "Browse..."))
					.ToolTipText(LOCTEXT("BrowseTip", "Pick a .bat, .cmd or .exe file to run when the queue finishes."))
					.OnClicked(this, &SAdvancedProjectLauncher::OnBrowsePostBuildClicked)
				]
			]

			// Overall progress
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
			[
				SNew(STextBlock)
				.Text(this, &SAdvancedProjectLauncher::GetOverallProgressText)
				.Font(FAppStyle::Get().GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
			[
				SNew(STextBlock)
				.Text(this, &SAdvancedProjectLauncher::GetStageText)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			// Live progress bar for the current profile
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)
			[
				SNew(SProgressBar)
				.Percent(this, &SAdvancedProjectLauncher::GetProgressBarPercent)
			]

			// Elapsed / estimated-remaining time
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(this, &SAdvancedProjectLauncher::GetTimingText)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			// Per-item status list
			+ SVerticalBox::Slot().FillHeight(0.22f).Padding(0, 0, 0, 6)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
				.Padding(2.0f)
				[
					SAssignNew(StatusListView, SListView<TSharedPtr<FQueueItem>>)
					.ListItemsSource(&StatusItems)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SAdvancedProjectLauncher::OnGenerateStatusRow)
				]
			]

			// Output log
			+ SVerticalBox::Slot().FillHeight(0.38f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
				.Padding(2.0f)
				[
					SAssignNew(LogBox, SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.AlwaysShowScrollbars(true)
				]
			]
		]
	];

	// Throttle log rebuilds so a noisy cook does not rebuild the text box on every line.
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SAdvancedProjectLauncher::OnLogRefreshTimer));

	// Initial sync so a tab reopened during a run shows live status/log immediately.
	HandleQueueChanged();
}

SAdvancedProjectLauncher::~SAdvancedProjectLauncher()
{
	if (Queue.IsValid() && QueueChangedHandle.IsValid())
	{
		Queue->OnChanged().Remove(QueueChangedHandle);
		QueueChangedHandle.Reset();
	}
	// Deliberately do NOT cancel the queue here: closing the tab must not abort the build.
}

void SAdvancedProjectLauncher::RefreshProfiles()
{
	ILauncherServicesModule& LauncherServices =
		FModuleManager::LoadModuleChecked<ILauncherServicesModule>("LauncherServices");
	ILauncherProfileManagerRef Manager = LauncherServices.GetProfileManager();

	// Preserve prior checkbox selection (keyed by profile id) across the refresh.
	TMap<FGuid, bool> PriorSelection;
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->Profile.IsValid())
		{
			PriorSelection.Add(It->Profile->GetId(), It->bSelected);
		}
	}

	ProfileItems.Reset();
	for (const ILauncherProfilePtr& Profile : Manager->GetAllProfiles())
	{
		if (!Profile.IsValid())
		{
			continue;
		}

		FAdvancedLauncherProfileItemPtr Item = MakeShared<FAdvancedLauncherProfileItem>();
		Item->Profile = Profile;
		Item->Name = Profile->GetName();
		Item->bValid = Profile->IsValidForLaunch();
		Item->Summary = FString::Printf(TEXT("%s  |  %s"),
			*Profile->GetProjectName(),
			LexToString(Profile->GetBuildConfiguration()));

		if (const bool* Was = PriorSelection.Find(Profile->GetId()))
		{
			Item->bSelected = *Was;
		}

		ProfileItems.Add(Item);
	}

	if (ProfileListView.IsValid())
	{
		ProfileListView->RequestListRefresh();
	}
}

TArray<ILauncherProfilePtr> SAdvancedProjectLauncher::CollectSelectedOrdered() const
{
	TArray<ILauncherProfilePtr> Out;
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->bSelected && It->bValid && It->Profile.IsValid())
		{
			Out.Add(It->Profile);
		}
	}
	return Out;
}

TSharedRef<ITableRow> SAdvancedProjectLauncher::OnGenerateProfileRow(
	FAdvancedLauncherProfileItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FAdvancedLauncherProfileItemPtr>, OwnerTable)
	[
		SNew(SHorizontalBox)

		// Checkbox
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 2)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([Item]()
			{
				return Item->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.IsEnabled_Lambda([Item, this]() { return Item->bValid && IsConfigEnabled(); })
			.OnCheckStateChanged_Lambda([Item](ECheckBoxState NewState)
			{
				Item->bSelected = (NewState == ECheckBoxState::Checked);
			})
		]

		// Name + summary
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(4, 2)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Name))
				.ColorAndOpacity(Item->bValid
					? FSlateColor::UseForeground()
					: FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Summary))
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
			]
		]

		// Invalid warning icon
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 2)
		[
			SNew(SImage)
			.Visibility(Item->bValid ? EVisibility::Collapsed : EVisibility::Visible)
			.Image(FAppStyle::Get().GetBrush("Icons.Warning"))
			.ToolTipText(LOCTEXT("InvalidProfile", "This profile is not valid for launch and cannot be queued."))
		]
	];
}

TSharedRef<ITableRow> SAdvancedProjectLauncher::OnGenerateStatusRow(
	TSharedPtr<FQueueItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FQueueItem>>, OwnerTable)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6, 2)
		[
			SNew(STextBlock).Text(FText::FromString(Item->ProfileName))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 2)
		[
			SNew(STextBlock)
			.Text_Lambda([Item]()
			{
				const FText StateText = AdvancedProjectLauncherUI::GetStateText(Item->State);
				if (Item->State == EQueueItemState::Succeeded
					|| Item->State == EQueueItemState::Failed
					|| Item->State == EQueueItemState::Canceled)
				{
					return FText::FromString(FString::Printf(TEXT("%s  (%s)"),
						*StateText.ToString(),
						*AdvancedProjectLauncherUI::FormatDuration(Item->DurationSeconds)));
				}
				return StateText;
			})
			.ColorAndOpacity_Lambda([Item]()
			{
				return AdvancedProjectLauncherUI::GetStateColor(Item->State);
			})
		]
	];
}

void SAdvancedProjectLauncher::SelectAll(bool bSelect)
{
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && (!bSelect || It->bValid))
		{
			It->bSelected = bSelect;
		}
	}
	if (ProfileListView.IsValid())
	{
		ProfileListView->RequestListRefresh();
	}
}

FReply SAdvancedProjectLauncher::OnMoveSelected(int32 Delta)
{
	if (!ProfileListView.IsValid() || Delta == 0)
	{
		return FReply::Handled();
	}

	TArray<FAdvancedLauncherProfileItemPtr> Selected = ProfileListView->GetSelectedItems();
	if (Selected.Num() == 0)
	{
		return FReply::Handled();
	}

	TArray<int32> Indices;
	for (const FAdvancedLauncherProfileItemPtr& Sel : Selected)
	{
		const int32 Idx = ProfileItems.IndexOfByKey(Sel);
		if (Idx != INDEX_NONE)
		{
			Indices.Add(Idx);
		}
	}
	Indices.Sort();

	// Moving down: process bottom-most first so items do not collide.
	if (Delta > 0)
	{
		Algo::Reverse(Indices);
	}

	for (int32 Idx : Indices)
	{
		const int32 Target = Idx + Delta;
		if (ProfileItems.IsValidIndex(Target))
		{
			ProfileItems.Swap(Idx, Target);
		}
	}

	ProfileListView->RequestListRefresh();
	ProfileListView->ClearSelection();
	for (const FAdvancedLauncherProfileItemPtr& Sel : Selected)
	{
		ProfileListView->SetItemSelection(Sel, true, ESelectInfo::Direct);
	}
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnStartClicked()
{
	if (Queue.IsValid() && !Queue->IsRunning())
	{
		Queue->Start(CollectSelectedOrdered());
	}
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnCancelClicked()
{
	if (Queue.IsValid())
	{
		Queue->Cancel();
	}
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnRefreshClicked()
{
	RefreshProfiles();
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnOpenBuildFolderClicked()
{
	FString Dir;

	// Prefer the package directory of the first selected profile.
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->bSelected && It->Profile.IsValid())
		{
			Dir = It->Profile->GetPackageDirectory();
			if (!Dir.IsEmpty())
			{
				break;
			}
		}
	}

	if (Dir.IsEmpty())
	{
		Dir = FPaths::ProjectDir();
	}
	Dir = FPaths::ConvertRelativePathToFull(Dir);

	if (!FPaths::DirectoryExists(Dir))
	{
		Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	FPlatformProcess::ExploreFolder(*Dir);
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnOpenProjectLauncherClicked()
{
	// "ProjectLauncher" is the tab id registered by the engine's Project Launcher module.
	FGlobalTabmanager::Get()->TryInvokeTab(FName("ProjectLauncher"));
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnBrowsePostBuildClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform || !Queue.IsValid())
	{
		return FReply::Handled();
	}

	const void* ParentWindowHandle = GetDialogParentWindowHandle();

	FString DefaultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (!Queue->GetPostBuildCommand().IsEmpty())
	{
		DefaultPath = FPaths::GetPath(Queue->GetPostBuildCommand());
	}

	TArray<FString> OutFiles;
	const bool bPicked = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Select a file to run when the build finishes"),
		DefaultPath,
		TEXT(""),
		TEXT("Scripts and programs (*.bat;*.cmd;*.exe)|*.bat;*.cmd;*.exe|All files (*.*)|*.*"),
		EFileDialogFlags::None,
		OutFiles);

	if (bPicked && OutFiles.Num() > 0)
	{
		const FString Picked = FPaths::ConvertRelativePathToFull(OutFiles[0]);
		Queue->SetPostBuildCommand(Picked);
		if (PostBuildBox.IsValid())
		{
			PostBuildBox->SetText(FText::FromString(Picked));
		}
	}

	return FReply::Handled();
}

void SAdvancedProjectLauncher::OnPostBuildTextCommitted(const FText& NewText, ETextCommit::Type /*CommitType*/)
{
	if (Queue.IsValid())
	{
		Queue->SetPostBuildCommand(NewText.ToString().TrimStartAndEnd());
	}
}

const void* SAdvancedProjectLauncher::GetDialogParentWindowHandle()
{
	// APIs present since UE4, so this is portable across UE 5.0-5.7.
	if (TSharedPtr<SWindow> OwnerWindow = FSlateApplication::Get().FindWidgetWindow(SharedThis(this)))
	{
		if (TSharedPtr<FGenericWindow> NativeWindow = OwnerWindow->GetNativeWindow())
		{
			return NativeWindow->GetOSWindowHandle();
		}
	}
	return nullptr;
}

void SAdvancedProjectLauncher::ShowNotification(const FText& Message, bool bSuccess)
{
	FNotificationInfo Info(Message);
	Info.ExpireDuration = bSuccess ? 4.0f : 6.0f;
	Info.bUseSuccessFailIcons = true;
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

bool SAdvancedProjectLauncher::IsExportEnabled() const
{
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->bSelected)
		{
			return true;
		}
	}
	return false;
}

FReply SAdvancedProjectLauncher::OnExportClicked()
{
	// Collect the checked profiles. The button is disabled when none are checked, but guard anyway.
	TArray<ILauncherProfilePtr> ToExport;
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->bSelected && It->Profile.IsValid())
		{
			ToExport.Add(It->Profile);
		}
	}
	if (ToExport.Num() == 0)
	{
		return FReply::Handled();
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	FString Folder;
	if (!DesktopPlatform->OpenDirectoryDialog(
			GetDialogParentWindowHandle(),
			TEXT("Choose a folder to export the selected profiles into"),
			FPaths::ProjectDir(),
			Folder)
		|| Folder.IsEmpty())
	{
		return FReply::Handled();
	}
	IFileManager::Get().MakeDirectory(*Folder, true);

	// One JSON file per selected profile, named after the profile (de-duplicated).
	TSet<FString> UsedNames;
	int32 Exported = 0;
	for (const ILauncherProfilePtr& Profile : ToExport)
	{
		FString BaseName = FPaths::MakeValidFileName(Profile->GetName(), TEXT('_'));
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("Profile");
		}
		FString FileName = BaseName;
		int32 Suffix = 1;
		while (UsedNames.Contains(FileName))
		{
			FileName = FString::Printf(TEXT("%s_%d"), *BaseName, ++Suffix);
		}
		UsedNames.Add(FileName);

		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		Profile->Save(Writer.Get()); // writes the full profile object (same format as a .ulp2)
		Writer->Close();

		if (FFileHelper::SaveStringToFile(Output, *(Folder / (FileName + TEXT(".json")))))
		{
			++Exported;
		}
	}

	ShowNotification(FText::Format(
		LOCTEXT("ExportOk", "Exported {0} profile(s) to {1}"),
		FText::AsNumber(Exported), FText::FromString(Folder)), Exported > 0);
	return FReply::Handled();
}

FReply SAdvancedProjectLauncher::OnImportClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	// Let the user pick one or more profile files (.json / .ulp2 are the same format).
	TArray<FString> OutFiles;
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		GetDialogParentWindowHandle(),
		TEXT("Import launch profiles"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Launch profiles (*.json;*.ulp2)|*.json;*.ulp2|All files (*.*)|*.*"),
		EFileDialogFlags::Multiple,
		OutFiles);
	if (!bOpened || OutFiles.Num() == 0)
	{
		return FReply::Handled();
	}

	ILauncherServicesModule& LauncherServices =
		FModuleManager::LoadModuleChecked<ILauncherServicesModule>("LauncherServices");
	ILauncherProfileManagerRef Manager = LauncherServices.GetProfileManager();

	int32 ImportedCount = 0;
	for (const FString& SelectedFile : OutFiles)
	{
		// LoadJSONProfile is the engine's own loader: it parses the file and resolves the
		// device group. It does NOT add the profile, so we register and persist it ourselves.
		ILauncherProfilePtr Profile = Manager->LoadJSONProfile(FPaths::ConvertRelativePathToFull(SelectedFile));
		if (Profile.IsValid())
		{
			Manager->AddProfile(Profile.ToSharedRef());      // replaces a profile with the same id
			Manager->SaveJSONProfile(Profile.ToSharedRef()); // persists to the current engine version
			++ImportedCount;
		}
	}

	RefreshProfiles();

	ShowNotification(FText::Format(
		LOCTEXT("ImportOk", "Imported {0} profile(s)."), FText::AsNumber(ImportedCount)),
		ImportedCount > 0);
	return FReply::Handled();
}

bool SAdvancedProjectLauncher::IsStartEnabled() const
{
	if (!Queue.IsValid() || Queue->IsRunning())
	{
		return false;
	}
	for (const FAdvancedLauncherProfileItemPtr& It : ProfileItems)
	{
		if (It.IsValid() && It->bSelected && It->bValid)
		{
			return true;
		}
	}
	return false;
}

bool SAdvancedProjectLauncher::IsCancelEnabled() const
{
	return Queue.IsValid() && Queue->IsRunning();
}

bool SAdvancedProjectLauncher::IsConfigEnabled() const
{
	return !(Queue.IsValid() && Queue->IsRunning());
}

ECheckBoxState SAdvancedProjectLauncher::GetStopOnFailureState() const
{
	const bool bStop = Queue.IsValid() ? Queue->GetStopOnFirstFailure() : true;
	return bStop ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SAdvancedProjectLauncher::OnStopOnFailureChanged(ECheckBoxState NewState)
{
	if (Queue.IsValid())
	{
		Queue->SetStopOnFirstFailure(NewState == ECheckBoxState::Checked);
	}
}

FText SAdvancedProjectLauncher::GetOverallProgressText() const
{
	if (!Queue.IsValid() || !Queue->IsRunning())
	{
		return LOCTEXT("Idle", "Idle");
	}

	const TArray<TSharedPtr<FQueueItem>>& Items = Queue->GetItems();
	const int32 Index = Queue->GetCurrentIndex();
	const FString Name = Items.IsValidIndex(Index) ? Items[Index]->ProfileName : FString();

	return FText::FromString(FString::Printf(TEXT("Profile %d of %d: %s"),
		Index + 1, Items.Num(), *Name));
}

FText SAdvancedProjectLauncher::GetStageText() const
{
	return Queue.IsValid() ? FText::FromString(Queue->GetCurrentStage()) : FText::GetEmpty();
}

TOptional<float> SAdvancedProjectLauncher::GetProgressBarPercent() const
{
	if (Queue.IsValid() && Queue->IsRunning())
	{
		const float Progress = Queue->GetCurrentProfileProgress();
		if (Progress >= 0.0f)
		{
			return TOptional<float>(Progress);
		}
		return TOptional<float>(); // unknown -> the progress bar animates (marquee)
	}
	return TOptional<float>(0.0f);
}

FText SAdvancedProjectLauncher::GetTimingText() const
{
	if (!Queue.IsValid() || !Queue->IsRunning())
	{
		return FText::GetEmpty();
	}

	const double Elapsed = Queue->GetCurrentProfileElapsedSeconds();
	const float Progress = Queue->GetCurrentProfileProgress();

	FString Result = FString::Printf(TEXT("Elapsed %s"), *AdvancedProjectLauncherUI::FormatClock(Elapsed));
	if (Progress > 0.02f && Progress < 1.0f)
	{
		const double Remaining = Elapsed * (1.0 - (double)Progress) / (double)Progress;
		Result += FString::Printf(TEXT("   -   ~%s remaining   (%d%%)"),
			*AdvancedProjectLauncherUI::FormatClock(Remaining),
			FMath::RoundToInt(Progress * 100.0f));
	}
	return FText::FromString(Result);
}

void SAdvancedProjectLauncher::HandleQueueChanged()
{
	// Game thread (the queue only ever broadcasts on the game thread).
	if (!Queue.IsValid())
	{
		return;
	}

	// Re-sync the status list only when the item set actually changes (cheap state changes
	// within an item are reflected via the row's polling lambdas).
	const TArray<TSharedPtr<FQueueItem>>& Live = Queue->GetItems();
	bool bSameSet = (StatusItems.Num() == Live.Num());
	if (bSameSet)
	{
		for (int32 i = 0; i < Live.Num(); ++i)
		{
			if (StatusItems[i] != Live[i])
			{
				bSameSet = false;
				break;
			}
		}
	}
	if (!bSameSet)
	{
		StatusItems = Live;
		if (StatusListView.IsValid())
		{
			StatusListView->RequestListRefresh();
		}
	}

	bLogDirty = true;
}

EActiveTimerReturnType SAdvancedProjectLauncher::OnLogRefreshTimer(double /*InCurrentTime*/, float /*InDeltaTime*/)
{
	if (bLogDirty && Queue.IsValid() && LogBox.IsValid())
	{
		LogBox->SetText(FText::FromString(FString::Join(Queue->GetLogLines(), TEXT("\n"))));
		LogBox->ScrollTo(ETextLocation::EndOfDocument);
		bLogDirty = false;
	}
	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
