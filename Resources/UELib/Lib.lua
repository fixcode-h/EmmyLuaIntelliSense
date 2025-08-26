

-- C++ 基础数据类型
---@class int8 : number
---@class uint8 : number
---@class int16 : number
---@class uint16 : number
---@class int32 : number
---@class uint32 : number
---@class int64 : number
---@class uint64 : number
---@class float : number
---@class double : number
---@class bool : boolean
---@class char : string
---@class wchar_t : string

-- UE 基础类型
---@class FText
---@field public DisplayString string

---@class FName
---@field public Name string

---@class FString
---@field public String string

---@class FDateTime
---@field public Ticks number

---@class FTimespan
---@field public Ticks number

---@class FGuid
---@field public A number
---@field public B number
---@field public C number
---@field public D number

-- UE 智能指针类型
---@class TSharedPtr
---@field public Get fun():any
---@field public IsValid fun():boolean
---@field public Reset fun()

---@class TWeakPtr
---@field public Pin fun():TSharedPtr
---@field public IsValid fun():boolean
---@field public Reset fun()

---@class TUniquePtr
---@field public Get fun():any
---@field public IsValid fun():boolean
---@field public Reset fun()

-- UE 软引用类型
---@class TSoftObjectPtr
---@field public Get fun():UObject
---@field public LoadSynchronous fun():UObject
---@field public IsValid fun():boolean
---@field public IsNull fun():boolean

---@class TSoftClassPtr
---@field public Get fun():UClass
---@field public LoadSynchronous fun():UClass
---@field public IsValid fun():boolean
---@field public IsNull fun():boolean

