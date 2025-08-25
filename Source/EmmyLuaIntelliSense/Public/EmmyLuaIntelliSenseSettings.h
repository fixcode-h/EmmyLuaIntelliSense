#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EmmyLuaIntelliSenseSettings.generated.h"

/**
 * 文件比较方法枚举
 */
UENUM(BlueprintType)
enum class EFileComparisonMethod : uint8
{
    /** 使用文件修改时间戳比较 */
    Timestamp UMETA(DisplayName = "File Timestamp"),
    
    /** 使用文件哈希值比较 */
    Hash UMETA(DisplayName = "File Hash")
};

/**
 * EmmyLua IntelliSense 插件设置
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Emmy Lua IntelliSense"))
class EMMYLUAINTELLISENSE_API UEmmyLuaIntelliSenseSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UEmmyLuaIntelliSenseSettings();

    /** 获取设置实例 */
    static const UEmmyLuaIntelliSenseSettings* Get();
    
    /** 获取可修改的设置实例 */
    static UEmmyLuaIntelliSenseSettings* GetMutable();

public:
    /** 文件比较方法 */
    UPROPERTY(EditAnywhere, config, Category = "Export Settings", 
        meta = (DisplayName = "File Comparison Method", 
                ToolTip = "Choose how to detect file changes: Timestamp (faster) or Hash (more accurate)"))
    EFileComparisonMethod FileComparisonMethod = EFileComparisonMethod::Timestamp;
    
    /** 是否导出蓝图文件 */
    UPROPERTY(EditAnywhere, config, Category = "Export Settings", 
        meta = (DisplayName = "Export Blueprints", 
                ToolTip = "Whether to export Blueprint files"))
    bool bExportBlueprintFiles = false;
    
    /** 是否启用增量导出 */
    UPROPERTY(EditAnywhere, config, Category = "Export Settings", 
        meta = (DisplayName = "Enable Incremental Export", 
                ToolTip = "Only export files that have been modified since last export"))
    bool bEnableIncrementalExport = true;
    
    /** 是否在启动时显示导出通知 */
    UPROPERTY(EditAnywhere, config, Category = "UI Settings", 
        meta = (DisplayName = "Show Export Notification on Startup", 
                ToolTip = "Show export notification when editor starts up"))
    bool bShowExportNotificationOnStartup = true;
    
    /** 导出通知显示时间（秒） */
    UPROPERTY(EditAnywhere, config, Category = "UI Settings", 
        meta = (DisplayName = "Notification Display Duration", 
                ToolTip = "How long to display the export notification (in seconds)", 
                ClampMin = "3.0", ClampMax = "30.0"))
    float NotificationDisplayDuration = 10.0f;
    
    /** 是否启用详细日志 */
    UPROPERTY(EditAnywhere, config, Category = "Debug Settings", 
        meta = (DisplayName = "Enable Verbose Logging", 
                ToolTip = "Enable detailed logging for debugging export issues"))
    bool bEnableVerboseLogging = false;

    // UDeveloperSettings interface
    virtual FName GetCategoryName() const override;
    virtual FText GetSectionText() const override;
    virtual FText GetSectionDescription() const override;
};