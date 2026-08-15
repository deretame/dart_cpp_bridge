// @ts-nocheck
import starlight from "@astrojs/starlight";
import { defineConfig } from "astro/config";
import starlightVersions from "starlight-versions";

/** Dev-only rewrite: / -> /dart_cpp_bridge/ so local dev can start from the root URL. */
function devRootRewrite() {
  return {
    name: "dev-root-rewrite",
    hooks: {
      "astro:server:setup"({ server }) {
        function onRequest(req, res) {
          const originalUrl = req.originalUrl ?? req.url ?? "/";
          if (originalUrl === "/" || originalUrl === "") {
            req.url = "/dart_cpp_bridge/";
            req.originalUrl = "/dart_cpp_bridge/";
          }
        }
        server.httpServer.prependListener("request", onRequest);
      },
    },
  };
}

export default defineConfig({
  site: "https://deretame.github.io",
  base: "/dart_cpp_bridge",
  integrations: [
    devRootRewrite(),
    starlight({
      title: "dart_cpp_bridge",
      defaultLocale: "root",
      locales: {
        root: { label: "English", lang: "en" },
        "zh-cn": { label: "简体中文", lang: "zh-CN" },
      },
      head: [],
      favicon: "/favicon.svg",
      customCss: ["./src/styles/custom.css"],
      plugins: [
        starlightVersions({
          versions: [{ slug: "v1", label: "v1" }],
          current: { label: "v2" },
        }),
      ],
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/deretame/dart_cpp_bridge",
        },
      ],
      sidebar: [
        {
          label: "Getting Started",
          slug: "getting-started",
          translations: { "zh-CN": "快速开始" },
        },
        {
          label: "Guides",
          translations: { "zh-CN": "指南" },
          items: [
            {
              label: "Fundamentals",
              translations: { "zh-CN": "基础" },
              items: [{ autogenerate: { directory: "guides/fundamentals" } }],
            },
            {
              label: "Advanced",
              translations: { "zh-CN": "进阶" },
              items: [{ autogenerate: { directory: "guides/advanced" } }],
            },
          ],
        },
        {
          label: "Code Generation",
          translations: { "zh-CN": "代码生成" },
          items: [{ autogenerate: { directory: "codegen" } }],
        },
        {
          label: "Reference",
          translations: { "zh-CN": "参考" },
          items: [{ autogenerate: { directory: "reference" } }],
        },
      ],
    }),
  ],
});
