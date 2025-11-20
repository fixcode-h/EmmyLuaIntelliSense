// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// 声明插件专属的日志类别
DECLARE_LOG_CATEGORY_EXTERN(LogEmmyLuaIntelliSense, Log, All);

class FEmmyLuaIntelliSenseModule : public IModuleInterface
{
public:
    // Begin IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    // End IModuleInterface

    void            ShowExportDialogIfNeeded();                                 // 如果有待处理的变更，显示导出对话框

private:
    void            OnPostEngineInit();                                        // 引擎初始化完成时调用
    void            OnAssetRegistryFilesLoaded();                              // 资源注册表文件加载完成时调用
    void            InitializeLuaExportManager();                               // 初始化Lua导出管理器
    void            RegisterSettings();                                         // 注册插件设置
    void            UnregisterSettings();                                      // 注销插件设置

private:
    bool            bIsInitialized = false;                                     // 防止多次初始化的标志
};
