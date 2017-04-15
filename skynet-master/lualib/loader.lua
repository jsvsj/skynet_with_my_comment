--loader的做用是以 cmdline参数为名去各项代码目录查找lua文件 找到后loadfile并执行(等效于dofile)  
--即boostrap为参数
--LUA_PATH LUA_CPATH LUA_SERVICE LUA_PRELOAD
--string.gmatch() 将返回一个迭代器，用于迭代所有出现在给定字符串中的匹配字符串

--...目前是boostrap

local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

--boostrap
SERVICE_NAME = args[1]

local main, pattern

--LUA_SERVICE=./service/?.lua
--即找到 ./service/boostrap.lua  执行loadfile(./service/boostrap.lua)
local err = {}
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do

	--gsub  做用是将所有符合匹配模式的地方都替换成替代字符串，并且返回替换后的字符串

	--如 /scs/?.lua;/csdc/cds/?/cs/a.lua  就?替换成SERVICE_NAME,
	local filename = string.gsub(pat, "?", SERVICE_NAME)

	--编译lua代码，编译成中间新式,返回被编译的chunk,被作为一个函数，可以用f()执行这个代码
	local f, msg = loadfile(filename)

	--如果f为空   msg是错误信息
	if not f then
		table.insert(err, msg)
	else
		pattern = pat        --./service/?.lua
		main = f
		break
	end
end

if not main then
	error(table.concat(err, "\n"))
end

LUA_SERVICE = nil
package.path , LUA_PATH = LUA_PATH
package.cpath , LUA_CPATH = LUA_CPATH
								 --./service/?.lua
local service_path = string.match(pattern, "(.*/)[^/?]+$")

if service_path then
		--./service/boostrap.lua
	service_path = string.gsub(service_path, "?", args[1])
	package.path = service_path .. "?.lua;" .. package.path
	SERVICE_PATH = service_path
else
	local p = string.match(pattern, "(.*/).+$")
	SERVICE_PATH = p
end

if LUA_PRELOAD then
	local f = assert(loadfile(LUA_PRELOAD))
	f(table.unpack(args))
	LUA_PRELOAD = nil
end

--main就是编译好的(调用 loadfile())那个lua代码,main()表示执行代码
--select(2, ...) 返回的是...可变参数中的第二个参数,而select('#',...)返回的是...可变参数的个数
--unpack()函数默认从下标1开始返回数组中所有元素
--select(2, table.unpack(args)) 做用是获取 args中的第二个元素值

--就是执行了 bootstrap.lua
main(select(2, table.unpack(args)))


