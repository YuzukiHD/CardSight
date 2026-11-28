// Sidebar of the site. A slug is the page path under src/content/docs/, without
// the extension; external entries point at the canonical repository files.
export const sidebar = [
	{
		label: 'Getting started',
		items: [
			{ label: 'Overview', slug: 'index' },
			{ label: 'Quick start', slug: 'getting-started/quick-start' },
			{ label: 'Flashing and boot', slug: 'getting-started/flashing' },
		],
	},
	{
		label: 'User guide',
		items: [
			{ label: 'On-screen UI', slug: 'guide/ui' },
			{ label: 'Card and files', slug: 'guide/card-files' },
			{ label: 'USB export', slug: 'guide/usb-export' },
		],
	},
	{
		label: 'Reference',
		items: [
			{ label: 'Shell commands', slug: 'reference/cli' },
			{ label: 'Configuration', slug: 'reference/configuration' },
			{ label: 'Hardware', slug: 'reference/hardware' },
			{ label: 'C API', slug: 'reference/api' },
		],
	},
	{
		label: 'Development',
		items: [
			{ label: 'Build', slug: 'development/build' },
			{ label: 'Testing', slug: 'development/testing' },
			{ label: 'Architecture', slug: 'development/architecture' },
			{ label: 'Storage proxy internals', slug: 'development/storage-proxy' },
			{ label: 'Porting', slug: 'development/porting' },
			{ label: 'Contributing', slug: 'development/contributing' },
			{ label: 'Maintaining', slug: 'development/maintaining' },
		],
	},
	{
		label: 'Project',
		items: [
			{ label: 'FAQ', slug: 'project/faq' },
			{ label: 'License', slug: 'project/license' },
			{
				label: 'Changelog',
				link: 'https://github.com/YuzukiHD/CardSight/blob/master/CHANGELOG.md',
			},
			{
				label: 'Security',
				link: 'https://github.com/YuzukiHD/CardSight/blob/master/SECURITY.md',
			},
			{
				label: 'Code of Conduct',
				link: 'https://github.com/YuzukiHD/CardSight/blob/master/CODE_OF_CONDUCT.md',
			},
			{
				label: 'Support',
				link: 'https://github.com/YuzukiHD/CardSight/blob/master/SUPPORT.md',
			},
		],
	},
];
