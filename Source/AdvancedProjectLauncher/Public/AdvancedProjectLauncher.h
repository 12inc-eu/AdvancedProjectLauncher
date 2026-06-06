// Copyright 12inc.eu Game Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FAdvancedLauncherQueue;
class FSpawnTabArgs;
class SDockTab;

/**
 * Editor module for the Advanced Project Launcher.
 *
 * Registers a nomad tab (Tools group + Tools menu entry) that hosts a widget for selecting
 * multiple existing launch profiles and building them sequentially. The sequential queue
 * controller is owned here (not by the widget) so a build keeps running even if the tab is
 * closed, and a reopened tab re-attaches to the running queue.
 */
class FAdvancedProjectLauncherModule : public IModuleInterface
{
public:

	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** The shared queue controller. Survives tab open/close for the whole editor session. */
	TSharedPtr<FAdvancedLauncherQueue> GetQueue() const { return Queue; }

private:

	/** Builds the dock tab content. */
	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

	/** Adds an entry to the toolbar "Platforms" menu, next to the built-in Project Launcher. */
	void RegisterMenus();

	/** Session-lifetime queue controller. */
	TSharedPtr<FAdvancedLauncherQueue> Queue;

	/** Handle for the deferred ToolMenus registration callback. */
	FDelegateHandle ToolMenusHandle;
};
