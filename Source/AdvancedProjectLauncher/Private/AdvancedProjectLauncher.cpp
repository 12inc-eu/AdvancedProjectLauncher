// Copyright 12inc.eu Game Studio. All Rights Reserved.

#include "AdvancedProjectLauncher.h"
#include "AdvancedLauncherQueue.h"
#include "SAdvancedProjectLauncher.h"

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"           // FUIAction / FExecuteAction
#include "ToolMenus.h"                             // extend the toolbar "Platforms" menu

#define LOCTEXT_NAMESPACE "FAdvancedProjectLauncherModule"

static const FName AdvancedProjectLauncherTabName("AdvancedProjectLauncher");

void FAdvancedProjectLauncherModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	// One controller for the whole editor session; reused across tab open/close so a build
	// keeps running even if the tab is closed.
	Queue = MakeShared<FAdvancedLauncherQueue>();

	// Register the tab in the same workspace group as the built-in Project Launcher
	// (Tools category). The Tools menu auto-populates that group sorted by display name, so
	// naming this "Project Launcher (Advanced)" makes it appear directly under "Project Launcher".
	// This is the same mechanism the engine's own Project Launcher uses (no separate menu entry).
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		AdvancedProjectLauncherTabName,
		FOnSpawnTab::CreateRaw(this, &FAdvancedProjectLauncherModule::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Project Launcher (Advanced)"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Queue multiple launch profiles and build them sequentially."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Launcher.TabIcon"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	// Add an entry to the toolbar "Platforms" menu (next to the built-in Project Launcher).
	ToolMenusHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAdvancedProjectLauncherModule::RegisterMenus));
}

void FAdvancedProjectLauncherModule::ShutdownModule()
{
	if (UObjectInitialized() && ToolMenusHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusHandle);
		ToolMenusHandle.Reset();
	}
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnregisterOwner(this);
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AdvancedProjectLauncherTabName);
	}

	// Drop our reference last. If a build is still running, the queue's own destructor
	// (CancelAndWait) handles the deterministic teardown; we do not block here.
	Queue.Reset();
}

TSharedRef<SDockTab> FAdvancedProjectLauncherModule::SpawnTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	DockTab->SetContent(
		SNew(SAdvancedProjectLauncher)
		.Queue(Queue));
	return DockTab;
}

void FAdvancedProjectLauncherModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Add our entry next to the built-in "Project Launcher", at the top of that section. The
	// "Platforms" menu's name and section changed between engine versions, so extend both: the
	// extension for whichever name does not exist on the running engine is simply never used.
	auto AddEntry = [](const TCHAR* MenuName, const TCHAR* SectionName)
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(MenuName);
		if (!Menu)
		{
			return;
		}

		FToolMenuSection& Section = Menu->FindOrAddSection(SectionName);
		FToolMenuEntry& Entry = Section.AddMenuEntry(
			"AdvancedProjectLauncher.Open",
			LOCTEXT("PlatformsMenuLabel", "Advanced Project Launcher..."),
			LOCTEXT("PlatformsMenuTooltip", "Open the Advanced Project Launcher to queue and build multiple profiles."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Launcher.TabIcon"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(AdvancedProjectLauncherTabName);
			})));
		Entry.InsertPosition = FToolMenuInsert(NAME_None, EToolMenuInsertType::First);
	};

	AddEntry(TEXT("UnrealEd.PlayWorldCommands.PlatformsMenu"), TEXT("TurnkeyOptions")); // UE 5.4 and earlier (toolbar Platforms dropdown)
	AddEntry(TEXT("LevelEditor.MainMenu.Platforms"), TEXT("ProjectLauncher"));          // UE 5.5+ (Platforms main menu)
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAdvancedProjectLauncherModule, AdvancedProjectLauncher)
