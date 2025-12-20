# 通知: 本项目已经正式迁移至 [AtomGit](https://atomgit.com/openeuler/memlink) 平台
# Memlink

## 介绍

在云场景，随着主机上的内存容量越来越多，内存所占的整机成本持续走高（超过50%），并且大部分主机内存的整体利用率较低，所以需要技术手段能够提升主机的内存利用率。在单机视角下，最大的挑战是如何充分回收虚拟机内部的空闲内存。由于虚拟化的隔离属性，Host是无法感知到Guest内部的内存语义（包括空闲、使用等），所以当前云场景上虚拟机的内存都是预占的，例如当前4U8G虚拟机在创建的时候就需要占用8G的大页内存，并且在运行过程中，一直都是占用8G的大页内存，即使虚拟机内部实际使用可能远低于8G。本特性提供Host感知Guest前端内存语义的通道，使Host能够感知到Guest内部的空闲内存分布，并能够回收这些空闲内存。

## 安装教程

1. 进入到项目目录
2. 安装必要的组件：yum-builddep memlinkd.spec
3. 将memlinkd打包：tar jcvf memlinkd.tar.bz2 --exclude=.git src
4. 将打包文档放到rpm构建目录下，如果目录没有需要自行创建：cp memlinkd.tar.bz2 /root/rpmbuild/SOURCES/
5. 执行构建：rpmbuild -ba memlinkd.spec
6. 安装memlink：cd /root/rpmbuild/RPMS/aarch64/;rpm -ivh memlinkd-*

## 使用说明

### 配置文件

1. memlink的配置文件位于/etc/memlinkd.conf
2. balloon_target_used_percent 取值[120, 10000]
   ,含义是利用虚拟机used内存计算出单虚拟机应该保留的内存量，例如,将该值配置为1.3，虚拟机规格=8G，used=4G，则target=4G *
   1.3=5.2G，即从虚拟机内部抽出8G – 5.2G = 2.8G。
3. balloon_target_max_total_percent 取值[0, 100]
   ，含义是配置保留给虚拟机的内存量的百分比，例如配置为60%则至少保留60%的内存给虚拟机。例如虚拟机规格=10G，则最少保留10G *
   60%(balloon_target_max_total_percent)=6G，即最多回收4G内存。
4. 配置文件示例如下

```conf
balloon_target_used_percent = 130
balloon_target_max_total_percent = 50
```

### 运行memlink

1. 启动memlink：systemctl start memlinkd
2. 虚拟机内存balloon的设置是在虚拟机的xml文件中配置balloon设备，需要启动虚拟机的时候在配置文件中添加如下字段

```xml

<domain type='kvm' id='1'>
    <memoryBacking>
        <hugepages>
            <page size='2' unit='MiB'/>
        </hugepages>
        <allocation mode="hugepage-ondemand"/>
    </memoryBacking>
    ......
    <devices>
        ......
        <memballoon model='virtio'>
            <stats period='5'/>
        </memballoon>
    </devices>
</domain>
```



