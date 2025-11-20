// Copyright Epic Games, Inc. All Rights Reserved.

#include "EmmyLuaIntelliSenseSettings.h"

UEmmyLuaIntelliSenseSettings::UEmmyLuaIntelliSenseSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("Emmy Lua IntelliSense");
}

const UEmmyLuaIntelliSenseSettings* UEmmyLuaIntelliSenseSettings::Get()
{
    return GetDefault<UEmmyLuaIntelliSenseSettings>();
}

UEmmyLuaIntelliSenseSettings* UEmmyLuaIntelliSenseSettings::GetMutable()
{
    return GetMutableDefault<UEmmyLuaIntelliSenseSettings>();
}

FName UEmmyLuaIntelliSenseSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}

FText UEmmyLuaIntelliSenseSettings::GetSectionText() const
{
    return NSLOCTEXT("EmmyLuaIntelliSenseSettings", "SectionText", "Emmy Lua IntelliSense");
}

FText UEmmyLuaIntelliSenseSettings::GetSectionDescription() const
{
    return NSLOCTEXT("EmmyLuaIntelliSenseSettings", "SectionDescription", "Configure Emmy Lua IntelliSense export settings");
}
