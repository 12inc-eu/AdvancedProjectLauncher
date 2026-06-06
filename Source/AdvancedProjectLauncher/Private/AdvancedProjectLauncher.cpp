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
#include "Framework/MultiBox/MultiBoxBuilder.h"   // FToolBarBuilder
#include "Framework/MultiBox/MultiBoxExtender.h"   // FExtender
#include "LevelEditor.h"                           // FLevelEditorModule toolbar extensibility

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

	// Level Editor toolbar button (mirrors ContextExporter's reliable FExtender approach).
	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		ToolbarExtender = MakeShared<FExtender>();
		ToolbarExtender->AddToolBarExtension(
			"Play",
			EExtensionHook::After,
			nullptr,
			FToolBarExtensionDelegate::CreateRaw(this, &FAdvancedProjectLauncherModule::FillToolbar));
		LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
}

void FAdvancedProjectLauncherModule::ShutdownModule()
{
	if (ToolbarExtender.IsValid() && FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		LevelEditorModule.GetToolBarExtensibilityManager()->RemoveExtender(ToolbarExtender);
	}
	ToolbarExtender.Reset();

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

void FAdvancedProjectLauncherModule::FillToolbar(FToolBarBuilder& Builder)
{
	Builder.BeginSection("AdvancedProjectLauncher");
	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(AdvancedProjectLauncherTabName);
		})),
		NAME_None,
		LOCTEXT("ToolbarButtonLabel", "Automated Build"),
		LOCTEXT("ToolbarButtonTooltip", "Open the Advanced Project Launcher to queue and build multiple profiles."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Launcher.TabIcon"));
	Builder.EndSection();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAdvancedProjectLauncherModule, AdvancedProjectLauncher)
