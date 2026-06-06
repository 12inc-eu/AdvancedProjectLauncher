// Copyright 12inc.eu Game Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "AdvancedLauncherProfileItem.h"

class FAdvancedLauncherQueue;
struct FQueueItem;
class ITableRow;
class STableViewBase;
class SMultiLineEditableTextBox;
class SEditableTextBox;
template <typename ItemType> class SListView;

/**
 * Main UI for the Advanced Project Launcher.
 *
 * Top: a checkbox list of every launch profile (with reorder + select-all). Middle: run
 * controls and a "stop on first failure" toggle. Bottom: overall progress, a per-item status
 * list, and a streaming output log. All state lives in the shared FAdvancedLauncherQueue passed
 * in via the Queue argument; this widget only reads it (on the game thread, via OnChanged).
 */
class SAdvancedProjectLauncher : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SAdvancedProjectLauncher) {}
		SLATE_ARGUMENT(TSharedPtr<FAdvancedLauncherQueue>, Queue)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SAdvancedProjectLauncher() override;

private:

	// Data
	void RefreshProfiles();
	TArray<ILauncherProfilePtr> CollectSelectedOrdered() const;

	// Profile list
	TSharedRef<ITableRow> OnGenerateProfileRow(FAdvancedLauncherProfileItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void SelectAll(bool bSelect);
	FReply OnMoveSelected(int32 Delta);

	// Status list
	TSharedRef<ITableRow> OnGenerateStatusRow(TSharedPtr<FQueueItem> Item, const TSharedRef<STableViewBase>& OwnerTable);

	// Run controls
	FReply OnStartClicked();
	FReply OnCancelClicked();
	FReply OnRefreshClicked();
	FReply OnOpenBuildFolderClicked();
	FReply OnOpenProjectLauncherClicked();

	// Post-build file (runs when the queue finishes)
	FReply OnBrowsePostBuildClicked();
	void OnPostBuildTextCommitted(const FText& NewText, ETextCommit::Type CommitType);

	// Export / import launch profiles as JSON (portable across engine versions and machines)
	FReply OnExportClicked();
	FReply OnImportClicked();

	/** Native window handle for parenting file dialogs (portable across UE 5.0-5.7). */
	const void* GetDialogParentWindowHandle();

	/** Show a transient editor toast notification. */
	void ShowNotification(const FText& Message, bool bSuccess);
	bool IsStartEnabled() const;
	bool IsCancelEnabled() const;
	bool IsConfigEnabled() const; // list/reorder enabled only when not running
	bool IsExportEnabled() const; // export enabled only when at least one profile is checked

	// Stop-on-failure toggle
	ECheckBoxState GetStopOnFailureState() const;
	void OnStopOnFailureChanged(ECheckBoxState NewState);

	// Progress text
	FText GetOverallProgressText() const;
	FText GetStageText() const;

	// Live progress bar + elapsed / estimated-remaining time for the current profile
	TOptional<float> GetProgressBarPercent() const;
	FText GetTimingText() const;

	// Queue change handler (game thread)
	void HandleQueueChanged();

	/** Throttled log refresh: rebuilds the log box at most a few times a second when dirty. */
	EActiveTimerReturnType OnLogRefreshTimer(double InCurrentTime, float InDeltaTime);

private:

	TSharedPtr<FAdvancedLauncherQueue> Queue;
	FDelegateHandle QueueChangedHandle;

	/** Set when the log changed; consumed by the active timer to avoid per-line rebuilds. */
	bool bLogDirty = false;

	/** Ordered, user-reorderable profile rows. Source for ProfileListView. */
	TArray<FAdvancedLauncherProfileItemPtr> ProfileItems;
	TSharedPtr<SListView<FAdvancedLauncherProfileItemPtr>> ProfileListView;

	/** Snapshot of the queue's items (shared ptrs), source for StatusListView. */
	TArray<TSharedPtr<FQueueItem>> StatusItems;
	TSharedPtr<SListView<TSharedPtr<FQueueItem>>> StatusListView;

	/** Read-only streaming log. */
	TSharedPtr<SMultiLineEditableTextBox> LogBox;

	/** Editable path of the file to run when the queue finishes. */
	TSharedPtr<SEditableTextBox> PostBuildBox;
};
