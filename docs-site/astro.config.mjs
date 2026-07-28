// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://deretame.github.io',
  base: '/dart_cpp_bridge',
  integrations: [
    starlight({
      title: 'dart_cpp_bridge',
      head: [],
      favicon: '/favicon.svg',
      customCss: ['./src/styles/custom.css'],
      social: [
        { icon: 'github', label: 'GitHub', href: 'https://github.com/deretame/dart_cpp_bridge' },
      ],
      sidebar: [
        {
          label: '指南',
          items: [{ autogenerate: { directory: 'guides' } }],
        },
        {
          label: 'Codegen',
          items: [{ autogenerate: { directory: 'codegen' } }],
        },
        {
          label: '参考',
          items: [{ autogenerate: { directory: 'reference' } }],
        },
      ],
    }),
  ],
});
