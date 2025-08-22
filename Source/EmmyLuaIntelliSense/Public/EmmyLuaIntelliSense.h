// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FEmmyLuaIntelliSenseModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Called when the engine has finished initialization */
	void OnPostEngineInit();
	
	/** Called when asset registry has finished loading all files */
	void OnAssetRegistryFilesLoaded();
	
	/** Initialize the Lua Export Manager */
	void InitializeLuaExportManager();
	
	/** Shows the export dialog if there are pending changes */
	void ShowExportDialogIfNeeded();
	
	/** Register plugin settings */
	void RegisterSettings();
	
	/** Unregister plugin settings */
	void UnregisterSettings();

private:
	/** Flag to prevent multiple initializations */
	bool bIsInitialized = false;
};
