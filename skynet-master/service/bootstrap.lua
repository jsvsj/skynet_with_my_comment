local skynet = require "skynet"
local harbor = require "skynet.harbor"
require "skynet.manager"	-- import skynet.launch, ...
local memory = require "memory"

skynet.start(function()
	local sharestring = tonumber(skynet.getenv "sharestring" or 4096)
	memory.ssexpand(sharestring)

	--会根据standalone配置项判断你启动的是一个master节点还是slave节点
	local standalone = skynet.getenv "standalone"

	--启动的launcher服务
	local launcher = assert(skynet.launch("snlua","launcher"))

	--给launcher命名为.launcher
	skynet.name(".launcher", launcher)

	--通过harbor是否配置0来判断你是否启动的是一个单节点skynet网络
	--单节点模式下，是不需要通过内置的harbor机制做节点通信的，但为了兼容(因为你还是有可能
	--注册全局名字)，需要启动一个叫做cdmmy的服务,他负责拦截对外广播的全局名字变更
	
	local harbor_id = tonumber(skynet.getenv "harbor" or 0)

	--如果是单节点
	if harbor_id == 0 then
		assert(standalone ==  nil)
		standalone = true
		skynet.setenv("standalone", "true")

		local ok, slave = pcall(skynet.newservice, "cdummy")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)

	else
		--如果是多节点模式，对应master节点，需要启动cmaster服务做节点调度用，此外，每个节点(
		--包括master节点自己)都需要启动cslave服务，用于节点间的消息转发，以及头部全局名字
		if standalone then
			if not pcall(skynet.newservice,"cmaster") then
				skynet.abort()
			end
		end

		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)
	end

	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end

	--接下来在master节点上，还需要启动DataCenter服务
	--然后启动用于UniqueService管理的service_mgr
	skynet.newservice "service_mgr"

	--最后，从config中读取start配置项，作为用户定义的服务启动入口脚本。成功后，把自己退出
	--默认为启动main
	
	--pcall 执行函数 pcall(函数,可变参数)
	--默认为启动main
	pcall(skynet.newservice,skynet.getenv "start" or "main")



	skynet.exit()
end)
