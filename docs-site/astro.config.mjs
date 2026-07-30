// @ts-nocheck
import starlight from "@astrojs/starlight";
import { defineConfig } from "astro/config";

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
      social: [
        {
          icon: "github",
          label: "GitHub",
          href: "https://github.com/deretame/dart_cpp_bridge",
        },
      ],
      sidebar: [
        { label: "Getting Started", slug: "getting-started" },
        {
          label: "Guides",
          items: [
            {
              label: "Fundamentals",
              items: [{ autogenerate: { directory: "guides/fundamentals" } }],
            },
            {
              label: "Advanced",
              items: [{ autogenerate: { directory: "guides/advanced" } }],
            },
          ],
        },
        {
          label: "Code Generation",
          items: [{ autogenerate: { directory: "codegen" } }],
        },
        {
          label: "Reference",
          items: [{ autogenerate: { directory: "reference" } }],
        },
      ],
    }),
  ],
});
