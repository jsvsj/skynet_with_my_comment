--loader���������� cmdline����Ϊ��ȥ�������Ŀ¼����lua�ļ� �ҵ���loadfile��ִ��(��Ч��dofile)  
--��boostrapΪ����
--LUA_PATH LUA_CPATH LUA_SERVICE LUA_PRELOAD
--string.gmatch() ������һ�������������ڵ������г����ڸ����ַ����е�ƥ���ַ���

--...Ŀǰ��boostrap

local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

--boostrap
SERVICE_NAME = args[1]

local main, pattern

--LUA_SERVICE=./service/?.lua
--���ҵ� ./service/boostrap.lua  ִ��loadfile(./service/boostrap.lua)
local err = {}
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do

	--gsub  �����ǽ����з���ƥ��ģʽ�ĵط����滻������ַ��������ҷ����滻����ַ���

	--�� /scs/?.lua;/csdc/cds/?/cs/a.lua  ��?�滻��SERVICE_NAME,
	local filename = string.gsub(pat, "?", SERVICE_NAME)

	--����lua���룬������м���ʽ,���ر������chunk,����Ϊһ��������������f()ִ���������
	local f, msg = loadfile(filename)

	--���fΪ��   msg�Ǵ�����Ϣ
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

--main���Ǳ���õ�(���� loadfile())�Ǹ�lua����,main()��ʾִ�д���
--select(2, ...) ���ص���...�ɱ�����еĵڶ�������,��select('#',...)���ص���...�ɱ�����ĸ���
--unpack()����Ĭ�ϴ��±�1��ʼ��������������Ԫ��
--select(2, table.unpack(args)) �����ǻ�ȡ args�еĵڶ���Ԫ��ֵ

--����ִ���� bootstrap.lua
main(select(2, table.unpack(args)))


