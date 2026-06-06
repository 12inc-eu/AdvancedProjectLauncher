// Copyright 12inc.eu Game Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ILauncher.h"
#include "ILauncherWorker.h"
#include "ILauncherProfile.h"

class ITargetDeviceProxyManager;

/** State of a single profile in the build queue. */
enum class EQueueItemState : uint8
{
	Pending,
	Building,
	Succeeded,
	Failed,
	Canceled
};

/** One entry in the build queue. Shared so the widget's status list can read live state. */
struct FQueueItem
{
	ILauncherProfilePtr Profile;
	FString ProfileName;
	EQueueItemState State = EQueueItemState::Pending;
	double DurationSeconds = 0.0;
	int32 ErrorCode = 0;
};

using FQueueItemPtr = TSharedPtr<FQueueItem>;

/** Fired on the GAME THREAD whenever queue state changes (progress, stage, item state, log). */
DECLARE_MULTICAST_DELEGATE(FOnQueueChanged);

/**
 * Drives a sequential build of multiple launch profiles.
 *
 * Owns one reused ILauncher and one device proxy manager (mirroring FProjectLauncherModel).
 * Launches profiles one at a time: the next one starts only after the previous worker reports
 * completion. The worker delegates fire on the worker thread, so every handler hops to the game
 * thread before touching state or starting the next launch. Lives on the module as a TSharedPtr,
 * so closing the widget tab does not abort an in-progress build.
 */
class FAdvancedLauncherQueue : public TSharedFromThis<FAdvancedLauncherQueue>
{
public:

	FAdvancedLauncherQueue();
	~FAdvancedLauncherQueue();

	/** True while a queue is executing. */
	bool IsRunning() const { return bRunning; }

	/** Index of the item currently building, or INDEX_NONE. */
	int32 GetCurrentIndex() const { return CurrentIndex; }

	/** The queued items (shared so the UI can read live per-item state). */
	const TArray<FQueueItemPtr>& GetItems() const { return Items; }

	/** Name of the stage the active worker last reported. */
	const FString& GetCurrentStage() const { return CurrentStage; }

	/** Seconds the current profile has been building (0 if not running). */
	double GetCurrentProfileElapsedSeconds() const;

	/** Progress of the current profile in [0,1], or -1 if unknown / not running. */
	float GetCurrentProfileProgress() const;

	/** The (capped) output log accumulated across the whole queue. */
	const TArray<FString>& GetLogLines() const { return LogLines; }

	bool GetStopOnFirstFailure() const { return bStopOnFirstFailure; }
	void SetStopOnFirstFailure(bool bInStop) { bStopOnFirstFailure = bInStop; }

	/**
	 * Optional file (.bat/.cmd/.exe) launched once when the queue finishes (not on cancel).
	 * Empty means nothing runs. Persisted to the editor's per-project config.
	 */
	const FString& GetPostBuildCommand() const { return PostBuildCommand; }
	void SetPostBuildCommand(const FString& InCommand);

	/** Begin a queue from an ordered list of profiles. No-op if already running or list empty. */
	void Start(const TArray<ILauncherProfilePtr>& OrderedProfiles);

	/** Request cancel of the active worker and stop advancing. */
	void Cancel();

	/** Subscribe to be notified (on the game thread) when queue state changes. */
	FOnQueueChanged& OnChanged() { return ChangedDelegate; }

private:

	/** Lazily create and cache the reused launcher and device proxy manager. */
	void EnsureLauncher();

	/** Launch the next pending item. Game thread only. */
	void StartNext();

	/** Append a line to the capped log buffer. Game thread only. */
	void AppendLog(const FString& Line);

	/** Bind the worker delegates (each marshals to the game thread). */
	void BindWorker(const ILauncherWorkerRef& Worker);

	/** Record a completed item and advance (or stop). Game thread only. */
	void HandleCompletedGameThread(bool bSucceeded, double Duration, int32 ErrorCode);

	/** Centralized end-of-queue: clears running state, runs the post-build file (unless canceled), broadcasts. */
	void FinishQueue(bool bWasCanceled);

	/** Launch the configured post-build file (used at end of queue). Returns true if it started. */
	bool RunPostBuildCommand();

	/** Parse a worker output line for progress info (e.g. the cook "Cooked packages X ... Total Z"). */
	void ParseProgressLine(const FString& Line);

	/** Play a built-in editor notification sound (respects the editor's "Enable Editor Sounds" setting). */
	void PlayQueueSound(const TCHAR* SoundObjectPath);

private:

	/** Created once, reused for every profile. */
	ILauncherPtr Launcher;
	TSharedPtr<ITargetDeviceProxyManager> DeviceProxyManager;

	TArray<FQueueItemPtr> Items;
	int32 CurrentIndex = INDEX_NONE;
	bool bRunning = false;
	bool bCancelRequested = false;
	bool bStopOnFirstFailure = true;

	/** Set once FinishQueue has run for the current queue, so the post-build file launches exactly once. */
	bool bQueueFinished = false;

	/** Optional file launched when the queue finishes (persisted to editor config). */
	FString PostBuildCommand;

	ILauncherWorkerPtr CurrentWorker;
	FString CurrentStage;

	/** FPlatformTime::Seconds() captured when the current profile started building. */
	double CurrentProfileStartSeconds = 0.0;

	/** Fine cook progress parsed from worker output (packages cooked / total). */
	int32 CookedPackages = 0;
	int32 CookTotalPackages = 0;

	TArray<FString> LogLines;
	static constexpr int32 MaxLogLines = 2000;

	FOnQueueChanged ChangedDelegate;
};
