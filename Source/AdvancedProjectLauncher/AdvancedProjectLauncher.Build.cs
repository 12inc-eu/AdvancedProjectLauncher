// Copyright 12inc.eu Game Studio. All Rights Reserved.

using UnrealBuildTool;

public class AdvancedProjectLauncher : ModuleRules
{
	public AdvancedProjectLauncher(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"Projects",
				"UnrealEd",
				"LevelEditor",              // toolbar button extender (mirrors ContextExporter)
				"WorkspaceMenuStructure",   // WorkspaceMenu::GetMenuStructure().GetToolsCategory()
				"LauncherServices",         // ILauncherServicesModule / ILauncher / profiles / worker
				"TargetDeviceServices",     // ITargetDeviceServicesModule -> device proxy manager
				"TargetPlatform",           // companion of LauncherServices (target platform types)
				"DesktopPlatform",          // file dialogs ("Open Build Folder", export/import)
				"Json"                      // export/import launch profiles as JSON
			}
		);
	}
}
