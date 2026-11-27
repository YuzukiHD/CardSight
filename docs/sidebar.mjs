// 侧边栏。直接从 YuzukiNeko 的 docs 结构改过来：组内 slug 是相对
// src/content/docs/ 的路径，页面文件和 slug 同名。
export const sidebar = [
	{
		label: '起步',
		items: [
			{ label: '项目简介', slug: 'index' },
			{ label: '功能一览', slug: 'features' },
			{ label: '构建', slug: 'build' },
			{ label: '烧写与启动', slug: 'fel' },
		],
	},
	{
		label: '使用',
		items: [
			{ label: '屏幕界面', slug: 'ui' },
			{ label: '命令行', slug: 'shell' },
			{ label: '存储代理与 USB', slug: 'proxy' },
			{ label: '文件系统', slug: 'filesystems' },
		],
	},
	{
		label: '硬件与实现',
		items: [
			{ label: '硬件说明', slug: 'hardware' },
			{ label: '软件架构', slug: 'architecture' },
			{ label: '移植与扩展', slug: 'porting' },
		],
	},
	{
		label: '项目',
		items: [
			{ label: '许可', slug: 'licensing' },
		],
	},
];
