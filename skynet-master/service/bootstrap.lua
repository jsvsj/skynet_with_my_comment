local skynet = require "skynet"
local harbor = require "skynet.harbor"
require "skynet.manager"	-- import skynet.launch, ...
local memory = require "memory"

skynet.start(function()
	local sharestring = tonumber(skynet.getenv "sharestring" or 4096)
	memory.ssexpand(sharestring)

	--�����standalone�������ж�����������һ��master�ڵ㻹��slave�ڵ�
	local standalone = skynet.getenv "standalone"

	--������launcher����
	local launcher = assert(skynet.launch("snlua","launcher"))

	--��launcher����Ϊ.launcher
	skynet.name(".launcher", launcher)

	--ͨ��harbor�Ƿ�����0���ж����Ƿ���������һ�����ڵ�skynet����
	--���ڵ�ģʽ�£��ǲ���Ҫͨ�����õ�harbor�������ڵ�ͨ�ŵģ���Ϊ�˼���(��Ϊ�㻹���п���
	--ע��ȫ������)����Ҫ����һ������cdmmy�ķ���,���������ض���㲥��ȫ�����ֱ��
	
	local harbor_id = tonumber(skynet.getenv "harbor" or 0)

	--����ǵ��ڵ�
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
		--����Ƕ�ڵ�ģʽ����Ӧmaster�ڵ㣬��Ҫ����cmaster�������ڵ�����ã����⣬ÿ���ڵ�(
		--����master�ڵ��Լ�)����Ҫ����cslave�������ڽڵ�����Ϣת�����Լ�ͷ��ȫ������
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

	--��������master�ڵ��ϣ�����Ҫ����DataCenter����
	--Ȼ����������UniqueService�����service_mgr
	skynet.newservice "service_mgr"

	--��󣬴�config�ж�ȡstart�������Ϊ�û�����ķ���������ڽű����ɹ��󣬰��Լ��˳�
	--Ĭ��Ϊ����main
	
	--pcall ִ�к��� pcall(����,�ɱ����)
	--Ĭ��Ϊ����main
	pcall(skynet.newservice,skynet.getenv "start" or "main")



	skynet.exit()
end)
