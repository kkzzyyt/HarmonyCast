# 更新检查镜像

默认更新检查地址为：

```text
https://gh-proxy.com/https://api.github.com/repos/kkzzyyt/HarmonyCast/releases/latest
```

它通过 GH-Proxy 中转项目的公开 GitHub Releases API，返回与原接口相同的版本信息。该服务的[中文使用文档](https://gh-proxy.com/docs/github-accelerator)包含官方 API 代理支持说明。本轮实测该地址返回 `v2.3.1` 及 7 个安装包资源；这只表示验证时可用，不保证各地区网络或后续服务状态。

镜像连接失败、单次请求超过 6 秒或返回的内容无法解析时，客户端自动尝试官方原地址：

```text
https://api.github.com/repos/kkzzyyt/HarmonyCast/releases/latest
```

两处都失败后才报告检查失败。设置中的「更新服务源」仍可填写自己的版本 JSON 地址；自定义地址不会自动经过该公共镜像，也不会自动改为其他公共更新源。更新检查的镜像设置只影响版本 API，安装包下载地址和各平台安装包选择保持版本信息中的原值。

回归测试使用本地 HTTP 服务器验证镜像错误和无效 JSON 回退、6 秒超时、旧检查取消以及自定义源隔离，不连接公网。
