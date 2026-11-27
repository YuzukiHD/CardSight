// Sidebar of the site. The structure follows the YuzukiNeko and lessampler docs:
// a slug is the page path relative to src/content/docs/, without the extension.
export const sidebar = [
	{
		label: 'Getting started',
		items: [
			{ label: 'Overview', slug: 'index' },
			{ label: 'Features', slug: 'features' },
			{ label: 'Build', slug: 'build' },
			{ label: 'Flashing and boot', slug: 'fel' },
		],
	},
	{
		label: 'Using CardSight',
		items: [
			{ label: 'On-screen UI', slug: 'ui' },
			{ label: 'Shell', slug: 'shell' },
			{ label: 'Storage proxy and USB', slug: 'proxy' },
			{ label: 'Filesystems', slug: 'filesystems' },
		],
	},
	{
		label: 'Hardware and internals',
		items: [
			{ label: 'Hardware', slug: 'hardware' },
			{ label: 'Architecture', slug: 'architecture' },
			{ label: 'Porting', slug: 'porting' },
		],
	},
	{
		label: 'Project',
		items: [
			{ label: 'License', slug: 'licensing' },
		],
	},
];
