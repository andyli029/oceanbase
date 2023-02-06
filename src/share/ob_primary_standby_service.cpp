/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STANDBY

#include "ob_primary_standby_service.h"              // ObPrimaryStandbyService
#include "lib/oblog/ob_log_module.h"              // LOG_*
#include "lib/utility/ob_print_utils.h"             // TO_STRING_KV
#include "rootserver/ob_cluster_event.h"          // CLUSTER_EVENT_ADD_CONTROL
#include "rootserver/ob_rs_event_history_table_operator.h" // ROOTSERVICE_EVENT_ADD
#include "rootserver/ob_tenant_role_transition_service.h" // ObTenantRoleTransitionService
#include "rootserver/ob_primary_ls_service.h"//ObTenantLSInfo
#include "share/restore/ob_log_restore_source_mgr.h"  // ObLogRestoreSourceMgr
#include "share/ls/ob_ls_recovery_stat_operator.h"// ObLSRecoveryStatOperator
#include "share/ls/ob_ls_life_manager.h" //ObLSLifeAgentManager
#include "share/ls/ob_ls_operator.h" //ObLSAttr
#include "storage/tx/ob_timestamp_service.h"  // ObTimestampService
#include "share/ob_standby_upgrade.h"  // ObStandbyUpgrade
#include "observer/ob_inner_sql_connection.h"//ObInnerSQLConnection
#include "storage/tx/ob_trans_service.h" //ObTransService

namespace oceanbase
{
using namespace oceanbase;
using namespace common;
using namespace obrpc;
using namespace share;
using namespace rootserver;

namespace standby
{

int ObPrimaryStandbyService::init(
           ObMySQLProxy *sql_proxy,
           share::schema::ObMultiVersionSchemaService *schema_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(sql_proxy)
      || OB_ISNULL(schema_service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), KP(sql_proxy), KP(schema_service));
  } else {
    sql_proxy_ = sql_proxy;
    schema_service_ = schema_service;
    inited_ = true;
  }
  return ret;
}

void ObPrimaryStandbyService::destroy()
{
  if (OB_UNLIKELY(!inited_)) {
    LOG_INFO("ObPrimaryStandbyService has been destroyed", K_(inited));
  } else {
    LOG_INFO("ObPrimaryStandbyService begin to destroy", K_(inited));
    sql_proxy_ = NULL;
    schema_service_ = NULL;
    inited_ = false;
    LOG_INFO("ObPrimaryStandbyService destroyed", K_(inited));
  }
}

int ObPrimaryStandbyService::check_inner_stat_()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret));
  } else if (OB_ISNULL(sql_proxy_) || OB_ISNULL(schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Member variables is NULL", KR(ret), KP(sql_proxy_), KP(schema_service_));
  }
  return ret;
}

int ObPrimaryStandbyService::switch_tenant(const obrpc::ObSwitchTenantArg &arg)
{
  int ret = OB_SUCCESS;
  int64_t begin_time = ObTimeUtility::current_time();
  uint64_t switch_tenant_id = OB_INVALID_ID;
  ObSchemaGetterGuard schema_guard;
  const char *alter_cluster_event = arg.get_alter_type_str();
  const ObSimpleTenantSchema *tenant_schema = nullptr;
  CLUSTER_EVENT_ADD_CONTROL_START(ret, alter_cluster_event, "stmt_str", arg.get_stmt_str());
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), KR(ret));
  } else if (OB_FAIL(get_target_tenant_id(arg.get_tenant_name(), arg.get_exec_tenant_id(), switch_tenant_id))) {
    LOG_WARN("failed to get_target_tenant_id", KR(ret), K(switch_tenant_id), K(arg));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("failed to get schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_tenant_info(switch_tenant_id, tenant_schema))) {
    LOG_WARN("failed to get tenant info", KR(ret), K(switch_tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant_schema is null", KR(ret), K(switch_tenant_id), K(arg));
  } else if (tenant_schema->is_normal()) {
    switch (arg.get_op_type()) {
      case ObSwitchTenantArg::SWITCH_TO_PRIMARY :
        if (OB_FAIL(switch_to_primary(switch_tenant_id, arg.get_op_type()))) {
          LOG_WARN("failed to switch_to_primary", KR(ret), K(switch_tenant_id), K(arg), KPC(tenant_schema));
        }
        break;
      case ObSwitchTenantArg::SWITCH_TO_STANDBY :
        if (OB_FAIL(switch_to_standby(switch_tenant_id, arg.get_op_type()))) {
          LOG_WARN("failed to switch_to_standby", KR(ret), K(switch_tenant_id), K(arg), KPC(tenant_schema));
        }
        break;
      case ObSwitchTenantArg::FAILOVER_TO_PRIMARY :
        if (OB_FAIL(failover_to_primary(switch_tenant_id, arg.get_op_type()))) {
          LOG_WARN("failed to failover_to_primary", KR(ret), K(switch_tenant_id), K(arg), KPC(tenant_schema));
        }
        break;
      default :
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unkown op_type", K(arg));
    }
  } else {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("tenant status is not normal, switch tenant is not allowed", KR(ret), K(switch_tenant_id), K(arg), KPC(tenant_schema));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant status is not normal, switch tenant is");
  }

  int64_t cost = ObTimeUtility::current_time() - begin_time;
  CLUSTER_EVENT_ADD_CONTROL_FINISH(ret, alter_cluster_event,
      K(cost),
      "stmt_str", arg.get_stmt_str());

  return ret;
}

int ObPrimaryStandbyService::failover_to_primary(const uint64_t tenant_id,
                                                 const obrpc::ObSwitchTenantArg::OpType &switch_optype)
{
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;
  ObSchemaGetterGuard schema_guard;
  const ObSimpleTenantSchema *tenant_schema = nullptr;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (OB_ISNULL(GCTX.srv_rpc_proxy_) || OB_ISNULL(schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pointer is null", KR(ret), KP(GCTX.srv_rpc_proxy_), KP(schema_service_));
  } else if (OB_UNLIKELY(obrpc::ObSwitchTenantArg::OpType::INVALID == switch_optype)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid switch_optype", KR(ret), K(switch_optype));
  } else if (!is_user_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("only support switch user tenant", KR(ret), K(tenant_id));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "tenant id, only support operating user tenant");
  } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(tenant_id, sql_proxy_,
                                                    false, tenant_info))) {
    LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
  } else if (tenant_info.is_primary() && tenant_info.is_normal_status()) {
    LOG_INFO("already is primary tenant, no need switch", K(tenant_info));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("failed to get schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("failed to get tenant info", KR(ret), K(tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant_schema is null", KR(ret), K(tenant_id));
  } else if (tenant_schema->is_normal()) {
    ObTenantRoleTransitionService role_transition_service(tenant_id, sql_proxy_, GCTX.srv_rpc_proxy_, switch_optype);
    if (OB_FAIL(role_transition_service.failover_to_primary())) {
      LOG_WARN("failed to failover to primary", KR(ret), K(tenant_id));
    }
  } else {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("tenant status is not normal, failover is not allowed", KR(ret), K(tenant_id), KPC(tenant_schema));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant status is not normal, failover is");
  }

  return ret;
}

int ObPrimaryStandbyService::get_target_tenant_id(const ObString &tenant_name,
                                                  const uint64_t exec_tenant_id,
                                                  uint64_t &switch_tenant_id)
{
  int ret = OB_SUCCESS;
  switch_tenant_id = OB_INVALID_ID;
  if (OB_INVALID_TENANT_ID == exec_tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(exec_tenant_id), KR(ret));
  } else if (tenant_name.empty()) {
    if (!is_user_tenant(exec_tenant_id)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("can't operate tenant without tenant name using SYS/meta tenant session", KR(ret), K(tenant_name), K(exec_tenant_id));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "omitting tenant name is ");
    } else {
      switch_tenant_id = exec_tenant_id;
    }
  } else {
    // tenant_name not empty
    if (OB_SYS_TENANT_ID != exec_tenant_id) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("can't specify tenant name using user tenant session", KR(ret), K(tenant_name), K(exec_tenant_id));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "tenant name, please don't specify tenant name");
    } else {
      if (OB_ISNULL(schema_service_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("invalid schema service", KR(ret), KP(schema_service_));
      } else {
        share::schema::ObSchemaGetterGuard guard;
        if (OB_FAIL(schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, guard))) {
          LOG_WARN("get_schema_guard failed", KR(ret));
        } else if (OB_FAIL(guard.get_tenant_id(tenant_name, switch_tenant_id))) {
          LOG_WARN("get_tenant_id failed", KR(ret), K(tenant_name), K(exec_tenant_id));
        } else if (!is_user_tenant(switch_tenant_id)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("only support switch user tenant", KR(ret), K(tenant_name), K(exec_tenant_id), K(switch_tenant_id));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "tenant name, only support operating user tenant");
        }
      }
    }
  }
  return ret;
}

int ObPrimaryStandbyService::recover_tenant(const obrpc::ObRecoverTenantArg &arg)
{
  int ret = OB_SUCCESS;
  int64_t begin_time = ObTimeUtility::current_time();
  uint64_t tenant_id = OB_INVALID_ID;
  const char *alter_cluster_event = "recover_tenant";
  CLUSTER_EVENT_ADD_CONTROL_START(ret, alter_cluster_event, "stmt_str", arg.get_stmt_str());
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), KR(ret));
  } else if (OB_FAIL(get_target_tenant_id(arg.get_tenant_name(), arg.get_exec_tenant_id(), tenant_id))) {
    LOG_WARN("failed to get_target_tenant_id", KR(ret), K(tenant_id), K(arg));
  } else if (OB_FAIL(do_recover_tenant(arg, tenant_id))) {
    LOG_WARN("failed to do_recover_tenant", KR(ret), K(tenant_id), K(arg));
  }

  int64_t cost = ObTimeUtility::current_time() - begin_time;
  CLUSTER_EVENT_ADD_CONTROL_FINISH(ret, alter_cluster_event,
      K(cost),
      "stmt_str", arg.get_stmt_str());

  return ret;
}

int ObPrimaryStandbyService::do_recover_tenant(const obrpc::ObRecoverTenantArg &arg, const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;
  ObSchemaGetterGuard schema_guard;
  const uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id);
  common::ObMySQLTransaction trans;
  const ObSimpleTenantSchema *tenant_schema = nullptr;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (!arg.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(arg), KR(ret));
  } else if (OB_ISNULL(GCTX.srv_rpc_proxy_) || OB_ISNULL(schema_service_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pointer is null", KR(ret), KP(GCTX.srv_rpc_proxy_), KP(schema_service_), KP(sql_proxy_));
  } else if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("failed to get schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("failed to get tenant info", KR(ret), K(tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant_schema is null", KR(ret), K(tenant_id), K(arg));
  } else if (OB_FAIL(trans.start(sql_proxy_, exec_tenant_id))) {
    LOG_WARN("failed to start trans", KR(ret), K(exec_tenant_id), K(tenant_id));
  } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(tenant_id, &trans, true, tenant_info))) {
    LOG_WARN("failed to load all tenant info", KR(ret), K(tenant_id));
  } else if (!tenant_info.is_standby()) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("tenant role is not STANDBY", K(tenant_info));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant role is not STANDBY, recover is");
  } else if (!tenant_info.is_normal_status()) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("tenant switchover_status is not NORMAL", K(tenant_info));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant switchover_status is not NORMAL, recover is");
  } else if (obrpc::ObRecoverTenantArg::RecoverType::UNTIL == arg.get_type()
              && (arg.get_recovery_until_scn() < tenant_info.get_sync_scn())) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("recover before sync_scn is not allow", KR(ret), K(tenant_info), K(tenant_id), K(arg));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "recover before sync_scn is");
  } else if (tenant_schema->is_normal()) {
    ObLogRestoreSourceMgr restore_source_mgr;
    const SCN &recovery_until_scn = obrpc::ObRecoverTenantArg::RecoverType::UNTIL == arg.get_type() ?
                                        arg.get_recovery_until_scn() : tenant_info.get_sync_scn();
    if (tenant_info.get_recovery_until_scn() == recovery_until_scn) {
      LOG_WARN("recovery_until_scn is same with original", KR(ret), K(tenant_info), K(tenant_id), K(arg));
    } else if (OB_FAIL(restore_source_mgr.init(tenant_id, &trans))) {
      LOG_WARN("failed to init restore_source_mgr", KR(ret), K(tenant_id), K(arg));
    } else if (OB_FAIL(restore_source_mgr.update_recovery_until_scn(recovery_until_scn))) {
      LOG_WARN("failed to update_recovery_until_scn", KR(ret), K(tenant_id), K(arg));
    } else if (OB_FAIL(ObAllTenantInfoProxy::update_tenant_recovery_until_scn(
                  tenant_id, trans, tenant_info.get_switchover_epoch(), recovery_until_scn))) {
      LOG_WARN("failed to update_tenant_recovery_until_scn", KR(ret), K(tenant_id), K(arg));
    }
  } else {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("tenant status is not normal, recover is not allowed", KR(ret), K(tenant_id), K(arg), KPC(tenant_schema));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant status is not normal, recover is");
  }

  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("failed to commit trans", KR(ret), KR(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }

  return ret;
}

int ObPrimaryStandbyService::switch_to_primary(
    const uint64_t tenant_id,
    const obrpc::ObSwitchTenantArg::OpType &switch_optype)
{
  int ret = OB_SUCCESS;
  int64_t begin_time = ObTimeUtility::current_time();
  ObAllTenantInfo tenant_info;
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (OB_ISNULL(GCTX.srv_rpc_proxy_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pointer is null", KR(ret), KP(GCTX.srv_rpc_proxy_), KP(sql_proxy_));
  } else if (OB_UNLIKELY(obrpc::ObSwitchTenantArg::OpType::INVALID == switch_optype)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid switch_optype", KR(ret), K(switch_optype));
  } else if (!is_user_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("only support switch user tenant", KR(ret), K(tenant_id));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "tenant id, only support operating user tenant");
  } else {
    ObTenantRoleTransitionService role_transition_service(tenant_id, sql_proxy_, GCTX.srv_rpc_proxy_, switch_optype);
    (void)role_transition_service.set_switchover_epoch(tenant_info.get_switchover_epoch());
    if (OB_FAIL(role_transition_service.failover_to_primary())) {
      LOG_WARN("failed to failover to primary", KR(ret), K(tenant_id));
    }
  }

  return ret;
}

int ObPrimaryStandbyService::switch_to_standby(
    const uint64_t tenant_id,
    const obrpc::ObSwitchTenantArg::OpType &switch_optype)
{
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;

  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (OB_ISNULL(GCTX.srv_rpc_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pointer is null", KR(ret), KP(GCTX.srv_rpc_proxy_));
  } else if (OB_UNLIKELY(obrpc::ObSwitchTenantArg::OpType::INVALID == switch_optype)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid switch_optype", KR(ret), K(switch_optype));
  } else if (!is_user_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("only support switch user tenant", KR(ret), K(tenant_id));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "tenant id, only support operating user tenant");
  } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(tenant_id, sql_proxy_, false, tenant_info))) {
    LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
  } else if (tenant_info.is_standby() && tenant_info.is_normal_status()) {
    LOG_INFO("already is standby tenant, no need switch", K(tenant_id), K(tenant_info));
  } else {
    switch(tenant_info.get_switchover_status().value()) {
      case share::ObTenantSwitchoverStatus::NORMAL_STATUS: {
        if (OB_FAIL(ret)) {
        } else if (!tenant_info.is_primary()) {
          ret = OB_OP_NOT_ALLOW;
          LOG_WARN("unexpected tenant role", KR(ret), K(tenant_info));
          LOG_USER_ERROR(OB_OP_NOT_ALLOW, "tenant role is not PRIMARY, switchover to standby is");
        } else if (OB_FAIL(update_tenant_status_before_sw_to_standby_(
                            tenant_info.get_switchover_status(),
                            tenant_info.get_tenant_role(),
                            tenant_info.get_switchover_epoch(),
                            tenant_id,
                            tenant_info))) {
          LOG_WARN("failed to update_tenant_status_before_sw_to_standby_", KR(ret), K(tenant_info),
                            K(tenant_id));
        }
      }
      case share::ObTenantSwitchoverStatus::PREPARE_SWITCHING_TO_STANDBY_STATUS: {
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(switch_to_standby_prepare_ls_status_(tenant_id,
                                                                tenant_info.get_switchover_status(),
                                                                tenant_info.get_switchover_epoch(),
                                                                tenant_info))) {
          LOG_WARN("failed to switch_to_standby_prepare_ls_status_", KR(ret), K(tenant_id), K(tenant_info));
        }
      }
      case share::ObTenantSwitchoverStatus::SWITCHING_TO_STANDBY_STATUS: {
        if (OB_FAIL(ret)) {
        } else {
          ObTenantRoleTransitionService role_transition_service(tenant_id, sql_proxy_, GCTX.srv_rpc_proxy_, switch_optype);

          (void)role_transition_service.set_switchover_epoch(tenant_info.get_switchover_epoch());
          if (OB_FAIL(role_transition_service.do_switch_access_mode_to_raw_rw(tenant_info, STANDBY_TENANT_ROLE))) {
            LOG_WARN("failed to do_switch_access_mode", KR(ret), K(tenant_id), K(tenant_info));
          } else if (OB_FAIL(role_transition_service.switchover_update_tenant_status(tenant_id,
                                                     false /* switch_to_standby */,
                                                     share::STANDBY_TENANT_ROLE,
                                                     tenant_info.get_switchover_status(),
                                                     share::NORMAL_SWITCHOVER_STATUS,
                                                     tenant_info.get_switchover_epoch(),
                                                     tenant_info))) {
            LOG_WARN("fail to switchover_update_tenant_status", KR(ret), K(tenant_id), K(tenant_info));
          } else {
            (void)role_transition_service.broadcast_tenant_info(
                  ObTenantRoleTransitionConstants::SWITCH_TO_STANDBY_LOG_MOD_STR);
          }
        }
        break;
      }
      default: {
        ret = OB_OP_NOT_ALLOW;
        LOG_WARN("switchover status not match", KR(ret), K(tenant_info), K(tenant_id));
        LOG_USER_ERROR(OB_OP_NOT_ALLOW, "switchover status not match, switchover to standby");
        break;
      }
    }
  }

  return ret;
}

int ObPrimaryStandbyService::update_tenant_status_before_sw_to_standby_(
    const ObTenantSwitchoverStatus cur_switchover_status,
    const ObTenantRole cur_tenant_role,
    const int64_t cur_switchover_epoch,
    const uint64_t tenant_id,
    ObAllTenantInfo &new_tenant_info)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObAllTenantInfo tenant_info;
  int64_t new_switchover_ts = common::OB_INVALID_TIMESTAMP;

  if (OB_UNLIKELY(!cur_switchover_status.is_valid()
                  || !cur_tenant_role.is_valid()
                  || !is_user_tenant(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", KR(ret), K(tenant_id), K(cur_switchover_status), K(cur_tenant_role));
  } else if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else {
    const uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id);
    if (OB_FAIL(trans.start(sql_proxy_, exec_tenant_id))) {
      LOG_WARN("fail to start trans", KR(ret), K(tenant_id));
    } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                    tenant_id, &trans, true, tenant_info))) {
      LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(!tenant_info.get_recovery_until_scn().is_max())) {
      ret = OB_OP_NOT_ALLOW;
      LOG_WARN("recovery_until_scn has been changed ", KR(ret), K(tenant_id), K(tenant_info));
      LOG_USER_ERROR(OB_OP_NOT_ALLOW, "recovery_until_scn has been changed, switchover to standby");
    } else if (cur_switchover_status != tenant_info.get_switchover_status()) {
      ret = OB_NEED_RETRY;
      LOG_WARN("tenant not expect switchover status", KR(ret), K(tenant_info), K(cur_switchover_status));
    } else if (cur_tenant_role != tenant_info.get_tenant_role()) {
      ret = OB_NEED_RETRY;
      LOG_WARN("tenant not expect tenant role", KR(ret), K(tenant_info), K(cur_tenant_role));
    } else if (cur_switchover_epoch != tenant_info.get_switchover_epoch()) {
      ret = OB_NEED_RETRY;
      LOG_WARN("tenant not expect switchover epoch", KR(ret), K(tenant_info), K(cur_switchover_epoch));
    } else if (OB_FAIL(ObAllTenantInfoProxy::update_tenant_role(
                  tenant_id, &trans, cur_switchover_epoch,
                  PRIMARY_TENANT_ROLE, cur_switchover_status,
                  share::PREP_SWITCHING_TO_STANDBY_SWITCHOVER_STATUS, new_switchover_ts))) {
      LOG_WARN("failed to update tenant role", KR(ret), K(tenant_id), K(cur_switchover_epoch), K(tenant_info));
    } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                    tenant_id, &trans, true, new_tenant_info))) {
      LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
    }
  }

  if (trans.is_started()) {
    int temp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (temp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("trans end failed", "is_commit", OB_SUCC(ret), KR(temp_ret));
      ret = OB_SUCC(ret) ? temp_ret : ret;
    }
  }

  CLUSTER_EVENT_ADD_LOG(ret, "update tenant before switchover to standby",
                  "tenant id", tenant_id,
                  "old switchover#", cur_switchover_epoch,
                  "new switchover#", tenant_info.get_switchover_epoch(),
                  K(cur_switchover_status), K(cur_tenant_role));
  return ret;
}

int ObPrimaryStandbyService::switch_to_standby_prepare_ls_status_(
    const uint64_t tenant_id,
    const ObTenantSwitchoverStatus &status,
    const int64_t switchover_epoch,
    ObAllTenantInfo &new_tenant_info)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObLSAttr sys_ls_attr;
  share::ObLSAttrOperator ls_operator(tenant_id, sql_proxy_);
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;

  if (!is_user_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema_service_ is NULL", KR(ret));
  } else if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("fail to get schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("failed to get tenant ids", KR(ret), K(tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_TENANT_NOT_EXIST;
    LOG_WARN("tenant not exist", KR(ret), K(tenant_id));
  } else {
    ObTenantLSInfo tenant_stat(GCTX.sql_proxy_, tenant_schema, tenant_id,
                               GCTX.srv_rpc_proxy_, GCTX.lst_operator_);
    /* lock SYS_LS to get accurate LS list, then fix ls status to make ls status consistency
       between __all_ls&__all_ls_status.
       Refer to ls operator, insert/update/delete of ls table are executed in the SYS_LS lock
       and normal switchover status */
    if (OB_FAIL(tenant_stat.process_ls_status_missmatch(true/* lock_sys_ls */,
                                   share::PREP_SWITCHING_TO_STANDBY_SWITCHOVER_STATUS))) {
      LOG_WARN("failed to process_ls_status_missmatch", KR(ret));
    } else if (OB_FAIL(ObAllTenantInfoProxy::update_tenant_switchover_status(
           tenant_id, sql_proxy_, switchover_epoch,
           status, share::SWITCHING_TO_STANDBY_SWITCHOVER_STATUS))) {
      LOG_WARN("failed to update tenant switchover status", KR(ret), K(tenant_id),
               K(switchover_epoch), K(status));
    } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                       tenant_id, sql_proxy_, false, new_tenant_info))) {
      LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(new_tenant_info.get_switchover_epoch() != switchover_epoch)) {
      ret = OB_NEED_RETRY;
      LOG_WARN("switchover is concurrency", KR(ret), K(switchover_epoch), K(new_tenant_info));
    }
  }

  return ret;
}

int ObPrimaryStandbyService::write_upgrade_barrier_log(
    ObMySQLTransaction &trans,
    const uint64_t tenant_id,
    const uint64_t data_version)
{
  int ret = OB_SUCCESS;
  ObStandbyUpgrade primary_data_version(data_version);
  observer::ObInnerSQLConnection *inner_conn = static_cast<observer::ObInnerSQLConnection *>(trans.get_connection());
  if (OB_FAIL(check_inner_stat_())) {
    LOG_WARN("inner stat error", KR(ret), K_(inited));
  } else if (OB_ISNULL(inner_conn)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("connection or trans service is null", KR(ret), KP(inner_conn));
  } else if (!is_user_tenant(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not user tenant_id", KR(ret), K(tenant_id));
  } else if (!ObClusterVersion::check_version_valid_(data_version)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid data_version", KR(ret), K(data_version));
  } else {
    const int64_t length = primary_data_version.get_serialize_size();
    char *buf = NULL;
    int64_t pos = 0;
    ObArenaAllocator allocator("StandbyUpgrade");
    if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(length)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc buf", KR(ret), K(length));
    } else if (OB_FAIL(primary_data_version.serialize(buf, length, pos))) {
      LOG_WARN("failed to serialize", KR(ret), K(primary_data_version), K(length), K(pos));
    } else if (OB_UNLIKELY(pos > length)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("serialize error", KR(ret), K(pos), K(length), K(primary_data_version));
    } else if (OB_FAIL(inner_conn->register_multi_data_source(
                       tenant_id, SYS_LS, transaction::ObTxDataSourceType::STANDBY_UPGRADE,
                       buf, length))) {
      LOG_WARN("failed to register tx data", KR(ret), K(tenant_id));
    }
    LOG_INFO("write_upgrade_barrier_log finished", KR(ret), K(tenant_id), K(primary_data_version), K(length), KPHEX(buf, length));
  }
  return ret;
}

}
}