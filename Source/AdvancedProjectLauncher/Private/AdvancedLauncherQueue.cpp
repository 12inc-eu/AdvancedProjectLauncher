// Copyright 12inc.eu Game Studio. All Rights Reserved.

#include "AdvancedLauncherQueue.h"

#include "ILauncherServicesModule.h"
#include "ITargetDeviceServicesModule.h"
#include "ITargetDeviceProxyManager.h"
#include "ILauncherTask.h"          // task list + status for progress
#include "Modules/ModuleManager.h"
#include "HAL/PlatformTime.h"       // elapsed time
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "CoreGlobals.h"
#include "Editor.h"                 // GEditor
#include "Editor/EditorEngine.h"    // UEditorEngine::PlayEditorSound

namespace
{
	static const TCHAR* APLConfigSection = TEXT("AdvancedProjectLauncher");

	// Built-in editor notification sounds (present and stable across UE 5.0-5.7).
	static const TCHAR* SoundQueueStarted    = TEXT("/Engine/EditorSounds/Notifications/CompileStart_Cue.CompileStart_Cue");
	static const TCHAR* SoundProjectFinished = TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue");
	static const TCHAR* SoundQueueFinished   = TEXT("/Engine/EditorSounds/GamePreview/StartPlayInEditor_Cue.StartPlayInEditor_Cue");
}

FAdvancedLauncherQueue::FAdvancedLauncherQueue()
{
	// Restore the persisted post-build file path.
	if (GConfig)
	{
		GConfig->GetString(APLConfigSection, TEXT("PostBuildCommand"), PostBuildCommand, GEditorPerProjectIni);
	}
}

FAdvancedLauncherQueue::~FAdvancedLauncherQueue()
{
	// If a build is still running at editor shutdown, block until the worker stops so the
	// worker thread does not outlive us. The weak guards in the bound lambdas already protect
	// against use-after-free, but CancelAndWait is the clean, deterministic teardown.
	if (CurrentWorker.IsValid())
	{
		CurrentWorker->CancelAndWait();
		CurrentWorker.Reset();
	}
}

void FAdvancedLauncherQueue::EnsureLauncher()
{
	if (Launcher.IsValid() && DeviceProxyManager.IsValid())
	{
		return;
	}

	ILauncherServicesModule& LauncherServices =
		FModuleManager::LoadModuleChecked<ILauncherServicesModule>("LauncherServices");
	ITargetDeviceServicesModule& DeviceServices =
		FModuleManager::LoadModuleChecked<ITargetDeviceServicesModule>("TargetDeviceServices");

	DeviceProxyManager = DeviceServices.GetDeviceProxyManager();
	Launcher = LauncherServices.CreateLauncher();
}

void FAdvancedLauncherQueue::Start(const TArray<ILauncherProfilePtr>& OrderedProfiles)
{
	check(IsInGameThread());

	if (bRunning || OrderedProfiles.Num() == 0)
	{
		return;
	}

	EnsureLauncher();

	Items.Reset();
	for (const ILauncherProfilePtr& Profile : OrderedProfiles)
	{
		if (!Profile.IsValid())
		{
			continue;
		}

		FQueueItemPtr Item = MakeShared<FQueueItem>();
		Item->Profile = Profile;
		Item->ProfileName = Profile->GetName();
		Item->State = EQueueItemState::Pending;
		Items.Add(Item);
	}

	if (Items.Num() == 0)
	{
		return;
	}

	bRunning = true;
	bCancelRequested = false;
	bQueueFinished = false;
	CurrentIndex = INDEX_NONE;
	LogLines.Reset();
	CurrentStage.Reset();

	AppendLog(FString::Printf(TEXT("Starting queue of %d profile(s)."), Items.Num()));
	PlayQueueSound(SoundQueueStarted);
	ChangedDelegate.Broadcast();

	StartNext();
}

void FAdvancedLauncherQueue::StartNext()
{
	check(IsInGameThread());

	if (bCancelRequested)
	{
		FinishQueue(/*bWasCanceled*/ true);
		return;
	}

	CurrentIndex++;

	if (!Items.IsValidIndex(CurrentIndex))
	{
		// Reached the end: the queue is finished.
		AppendLog(TEXT("Queue finished."));
		FinishQueue(/*bWasCanceled*/ false);
		return;
	}

	const FQueueItemPtr Item = Items[CurrentIndex];

	// Validity can change between Refresh and Start, so re-check here.
	if (!Item->Profile.IsValid() || !Item->Profile->IsValidForLaunch())
	{
		Item->State = EQueueItemState::Failed;
		Item->ErrorCode = -1;
		AppendLog(FString::Printf(TEXT("Profile '%s' is not valid for launch; skipping."), *Item->ProfileName));
		ChangedDelegate.Broadcast();

		if (bStopOnFirstFailure)
		{
			AppendLog(TEXT("Stopping because a profile failed (stop on first failure is on)."));
			FinishQueue(/*bWasCanceled*/ false);
			return;
		}

		StartNext();
		return;
	}

	Item->State = EQueueItemState::Building;
	CurrentStage = TEXT("Starting...");
	CurrentProfileStartSeconds = FPlatformTime::Seconds();
	CookedPackages = 0;
	CookTotalPackages = 0;
	AppendLog(FString::Printf(TEXT("[%d/%d] Building '%s'."), CurrentIndex + 1, Items.Num(), *Item->ProfileName));
	ChangedDelegate.Broadcast();

	ILauncherWorkerPtr Worker = Launcher->Launch(DeviceProxyManager.ToSharedRef(), Item->Profile.ToSharedRef());

	if (!Worker.IsValid())
	{
		// Launch failed to even start a worker: treat as an immediate failure.
		Item->State = EQueueItemState::Failed;
		Item->ErrorCode = -1;
		AppendLog(FString::Printf(TEXT("Failed to start launch for '%s'."), *Item->ProfileName));
		ChangedDelegate.Broadcast();

		if (bStopOnFirstFailure)
		{
			AppendLog(TEXT("Stopping because a profile failed (stop on first failure is on)."));
			FinishQueue(/*bWasCanceled*/ false);
			return;
		}

		StartNext();
		return;
	}

	CurrentWorker = Worker;
	BindWorker(Worker.ToSharedRef());
}

void FAdvancedLauncherQueue::BindWorker(const ILauncherWorkerRef& Worker)
{
	// Capture a weak ptr to THIS queue. If the queue is destroyed while the worker thread is
	// still firing, the AsyncTask body simply no-ops. We never capture the widget.
	TWeakPtr<FAdvancedLauncherQueue> WeakSelf = AsShared();

	// OUTPUT: worker thread -> game thread, append and broadcast.
	Worker->OnOutputReceived().AddLambda([WeakSelf](const FString& Message)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Message]()
		{
			if (TSharedPtr<FAdvancedLauncherQueue> Self = WeakSelf.Pin())
			{
				Self->ParseProgressLine(Message);
				Self->AppendLog(Message);
				Self->ChangedDelegate.Broadcast();
			}
		});
	});

	// STAGE STARTED: worker thread -> game thread.
	Worker->OnStageStarted().AddLambda([WeakSelf](const FString& StageName)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, StageName]()
		{
			if (TSharedPtr<FAdvancedLauncherQueue> Self = WeakSelf.Pin())
			{
				Self->CurrentStage = StageName;
				Self->ChangedDelegate.Broadcast();
			}
		});
	});

	// STAGE COMPLETED: worker thread -> game thread.
	Worker->OnStageCompleted().AddLambda([WeakSelf](const FString& StageName, double /*Time*/)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, StageName]()
		{
			if (TSharedPtr<FAdvancedLauncherQueue> Self = WeakSelf.Pin())
			{
				Self->AppendLog(FString::Printf(TEXT("Stage complete: %s"), *StageName));
				Self->ChangedDelegate.Broadcast();
			}
		});
	});

	// CANCELED: worker thread -> game thread. Mark the current item canceled and stop.
	Worker->OnCanceled().AddLambda([WeakSelf](double /*Time*/)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
		{
			if (TSharedPtr<FAdvancedLauncherQueue> Self = WeakSelf.Pin())
			{
				if (Self->Items.IsValidIndex(Self->CurrentIndex)
					&& Self->Items[Self->CurrentIndex]->State == EQueueItemState::Building)
				{
					Self->Items[Self->CurrentIndex]->State = EQueueItemState::Canceled;
				}
				Self->AppendLog(TEXT("Canceled."));
				Self->FinishQueue(/*bWasCanceled*/ true);
			}
		});
	});

	// COMPLETED: records the result AND starts the next launch. Must hop to the game thread.
	Worker->OnCompleted().AddLambda([WeakSelf](bool bSucceeded, double Duration, int32 ErrorCode)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bSucceeded, Duration, ErrorCode]()
		{
			if (TSharedPtr<FAdvancedLauncherQueue> Self = WeakSelf.Pin())
			{
				Self->HandleCompletedGameThread(bSucceeded, Duration, ErrorCode);
			}
		});
	});
}

void FAdvancedLauncherQueue::HandleCompletedGameThread(bool bSucceeded, double Duration, int32 ErrorCode)
{
	check(IsInGameThread());

	// A cancel may have already finished this item via OnCanceled. If so, do not advance.
	if (Items.IsValidIndex(CurrentIndex))
	{
		FQueueItemPtr Item = Items[CurrentIndex];
		if (Item->State == EQueueItemState::Building)
		{
			Item->DurationSeconds = Duration;
			Item->ErrorCode = ErrorCode;
			Item->State = bSucceeded ? EQueueItemState::Succeeded : EQueueItemState::Failed;
			AppendLog(FString::Printf(TEXT("[%d/%d] '%s' %s (%.0f s, code %d)."),
				CurrentIndex + 1, Items.Num(), *Item->ProfileName,
				bSucceeded ? TEXT("succeeded") : TEXT("failed"), Duration, ErrorCode));
		}
	}

	CurrentWorker.Reset();
	CurrentStage.Reset();
	ChangedDelegate.Broadcast();

	if (bCancelRequested)
	{
		FinishQueue(/*bWasCanceled*/ true);
		return;
	}

	if (!bSucceeded && bStopOnFirstFailure)
	{
		AppendLog(TEXT("Stopping because a profile failed (stop on first failure is on)."));
		FinishQueue(/*bWasCanceled*/ false);
		return;
	}

	// One profile done with at least one more to go: signal the transition to the next build.
	// (The last profile does not play this; reaching the end plays the final sound instead.)
	if (Items.IsValidIndex(CurrentIndex + 1))
	{
		PlayQueueSound(SoundProjectFinished);
	}

	StartNext();
}

void FAdvancedLauncherQueue::Cancel()
{
	check(IsInGameThread());

	if (!bRunning)
	{
		return;
	}

	bCancelRequested = true;

	if (CurrentWorker.IsValid())
	{
		// Non-blocking. The OnCanceled / OnCompleted delegate finishes the bookkeeping.
		AppendLog(TEXT("Cancel requested..."));
		ChangedDelegate.Broadcast();
		CurrentWorker->Cancel();
	}
	else
	{
		FinishQueue(/*bWasCanceled*/ true);
	}
}

void FAdvancedLauncherQueue::AppendLog(const FString& Line)
{
	check(IsInGameThread());

	LogLines.Add(Line);

	// Trim in batches to avoid frequent reallocations. The 2-arg RemoveAt (default shrinking)
	// is portable across UE 5.0-5.7; the EAllowShrinking enum only exists in newer engines.
	if (LogLines.Num() > MaxLogLines + 256)
	{
		LogLines.RemoveAt(0, LogLines.Num() - MaxLogLines);
	}
}

void FAdvancedLauncherQueue::ParseProgressLine(const FString& Line)
{
	check(IsInGameThread());

	// Cook progress, e.g. "LogCook: Display: Cooked packages 6047 Packages Remain 3076 Total 9123".
	const int32 CookedIdx = Line.Find(TEXT("Cooked packages "), ESearchCase::IgnoreCase);
	if (CookedIdx == INDEX_NONE)
	{
		return;
	}
	const int32 TotalIdx = Line.Find(TEXT("Total "), ESearchCase::IgnoreCase, ESearchDir::FromStart, CookedIdx);
	if (TotalIdx == INDEX_NONE)
	{
		return;
	}

	const int32 Cooked = FCString::Atoi(*Line.Mid(CookedIdx + 16)); // len("Cooked packages ") == 16
	const int32 Total = FCString::Atoi(*Line.Mid(TotalIdx + 6));    // len("Total ") == 6
	if (Total > 0)
	{
		CookedPackages = Cooked;
		CookTotalPackages = Total;
	}
}

double FAdvancedLauncherQueue::GetCurrentProfileElapsedSeconds() const
{
	if (bRunning && CurrentProfileStartSeconds > 0.0)
	{
		return FPlatformTime::Seconds() - CurrentProfileStartSeconds;
	}
	return 0.0;
}

float FAdvancedLauncherQueue::GetCurrentProfileProgress() const
{
	if (!bRunning || !CurrentWorker.IsValid())
	{
		return -1.0f;
	}

	TArray<ILauncherTaskPtr> Tasks;
	CurrentWorker->GetTasks(Tasks);
	if (Tasks.Num() == 0)
	{
		return -1.0f;
	}

	// Find the cook task (the long, package-counted phase). Weight the bar so build is the
	// first 10%, cook is the middle 80% (driven by cooked/total), and packaging is the last 10%.
	int32 CookIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Tasks.Num(); ++Index)
	{
		if (Tasks[Index].IsValid() && Tasks[Index]->GetName().Contains(TEXT("Cook")))
		{
			CookIndex = Index;
			break;
		}
	}

	auto CountCompleted = [&Tasks](int32 First, int32 Last) -> int32
	{
		int32 Done = 0;
		for (int32 Index = First; Index < Last; ++Index)
		{
			if (Tasks[Index].IsValid() && Tasks[Index]->GetStatus() == ELauncherTaskStatus::Completed)
			{
				++Done;
			}
		}
		return Done;
	};

	if (CookIndex == INDEX_NONE)
	{
		// No identifiable cook task: fall back to a coarse completed/total ratio.
		return FMath::Clamp((float)CountCompleted(0, Tasks.Num()) / (float)Tasks.Num(), 0.0f, 1.0f);
	}

	const ELauncherTaskStatus::Type CookStatus = Tasks[CookIndex]->GetStatus();

	if (CookStatus == ELauncherTaskStatus::Pending)
	{
		// Pre-cook (build/prepare) phase: 0 .. 0.10.
		const float PreFrac = (CookIndex > 0) ? (float)CountCompleted(0, CookIndex) / (float)CookIndex : 0.0f;
		return 0.10f * PreFrac;
	}

	if (CookStatus == ELauncherTaskStatus::Busy)
	{
		// Cook phase: 0.10 .. 0.90, driven by cooked/total packages when available.
		const float CookFrac = (CookTotalPackages > 0)
			? FMath::Clamp((float)CookedPackages / (float)CookTotalPackages, 0.0f, 1.0f)
			: 0.0f;
		return 0.10f + 0.80f * CookFrac;
	}

	// Cook finished: packaging/deploy phase: 0.90 .. 1.0.
	const int32 PostTotal = Tasks.Num() - 1 - CookIndex;
	const float PostFrac = (PostTotal > 0) ? (float)CountCompleted(CookIndex + 1, Tasks.Num()) / (float)PostTotal : 1.0f;
	return 0.90f + 0.10f * PostFrac;
}

void FAdvancedLauncherQueue::FinishQueue(bool bWasCanceled)
{
	check(IsInGameThread());

	// Run the finish logic exactly once per queue. This guards against FinishQueue being
	// reached more than once (e.g. a late worker callback after the queue already ended), so
	// the post-build file is launched a single time, never in a loop.
	if (bQueueFinished)
	{
		return;
	}
	bQueueFinished = true;

	bRunning = false;
	CurrentWorker.Reset();
	CurrentStage.Reset();

	// On any real finish (success or failure, but not user cancel): play the final sound
	// and launch the post-build file.
	if (!bWasCanceled)
	{
		PlayQueueSound(SoundQueueFinished);
		RunPostBuildCommand();
	}

	ChangedDelegate.Broadcast();
}

bool FAdvancedLauncherQueue::RunPostBuildCommand()
{
	check(IsInGameThread());

	if (PostBuildCommand.IsEmpty())
	{
		return false;
	}

	FString FullPath = FPaths::ConvertRelativePathToFull(PostBuildCommand);
	if (!FPaths::FileExists(FullPath))
	{
		AppendLog(FString::Printf(TEXT("Post-build file not found: %s"), *FullPath));
		ChangedDelegate.Broadcast();
		return false;
	}

#if PLATFORM_WINDOWS
	FullPath.ReplaceInline(TEXT("/"), TEXT("\\"));
#endif

	// Launch it exactly the way double-clicking the file does (ShellExecute). This gives a .bat
	// its own real, interactive console window, so scripts that prompt for input (Y/N, Steam
	// login, etc.) work. Launching it detached through "cmd /c" gives no usable stdin, which
	// makes an interactive .bat spin forever on its prompts (the "endless windows" bug).
	const bool bLaunched = FPlatformProcess::LaunchFileInDefaultExternalApplication(
		*FullPath, nullptr, ELaunchVerb::Open, /*bPromptToOpenOnFailure*/ false);

	AppendLog(bLaunched
		? FString::Printf(TEXT("Launched post-build file: %s"), *FullPath)
		: FString::Printf(TEXT("Failed to launch post-build file: %s"), *FullPath));
	ChangedDelegate.Broadcast();
	return bLaunched;
}

void FAdvancedLauncherQueue::PlayQueueSound(const TCHAR* SoundObjectPath)
{
	// Editor-only. PlayEditorSound honors the "Enable Editor Sounds" preference, so this is
	// silently skipped when the user has editor sounds turned off.
	if (GEditor && SoundObjectPath)
	{
		GEditor->PlayEditorSound(FString(SoundObjectPath));
	}
}

void FAdvancedLauncherQueue::SetPostBuildCommand(const FString& InCommand)
{
	PostBuildCommand = InCommand;

	if (GConfig)
	{
		GConfig->SetString(APLConfigSection, TEXT("PostBuildCommand"), *PostBuildCommand, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
}
