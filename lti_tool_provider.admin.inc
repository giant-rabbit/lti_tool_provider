<?php

/**
 * @file
 * Admin forms and menu page callbacks for LTI Tool Provider module.
 */

/**
 * LTI launch.
 * 
 * Implements the [base_url]/lti path which is called to launch the
 * tool from the LMS
 *  - Verifies the request using OAuth and adds the lti context_info
 *    to the session
 *  - Authenticates the user, possibly after provisioning the account
 *  - Authorises the user via global role mapping
 *  - If OG is configured a course may be provisioned from the
 *    context_info and group roles are mapped
 *  - Finally the destination is calculated and user is redirected there.
 */
function lti_tool_provider_launch() {
  global $user;
  // If not an LTI launch request, then ignore.
  if (!lti_tool_provider_is_basic_lti_request()) {
    drupal_set_message(t('Not a LTI request.'), 'info');
    return 'Error: Not a LTI request.';
  }
  // Insure we have a valid context.
  if (empty($_REQUEST['oauth_consumer_key'])) {
    unset($_SESSION['lti_tool_provider_context_info']);
    drupal_set_message(t('Not a valid LTI context.'), 'info');
    return 'Error: Invalid context. Missing oauth_consumer_key in request';
  }
  // Begin a new session based on this LTI launch request.
  drupal_session_start();
  $oauth_consumer_key = $_REQUEST["oauth_consumer_key"];
  // Verify the message signature.
  $store = new LTIToolProviderOAuthDataStore();
  $server = new OAuthServer($store);
  $method = new OAuthSignatureMethod_HMAC_SHA1();
  $server->add_signature_method($method);
  $request = OAuthRequest::from_request();
  try {
    $server->verify_request($request);
    // This is a LTI launch request.
  } catch (Exception $e) {
    drupal_set_message($e->getMessage(), 'error');
    return 'Error: Invalid context, OAuth failure.';
  }
  // Collect the launch information for later storage in the session.
  $launch_info = $request->get_parameters();
  $domain = lti_tool_provider_retrieve_consumer_domain($oauth_consumer_key);
  if (isset($launch_info['context_id']) && isset($launch_info['user_id'])) {
    $lti_user = $launch_info['user_id'] . $domain;
  }
  else {
    $lti_user = 'lti_user' . $domain;
  }
  // Revalidate incoming user.
  if ($user->uid > 0 && $user->name != $lti_user) {
    // Log out prior user and clear session, retaining lti session variables.
    watchdog('user', 'Session closed for %name.', array('%name' => $user->name));
    module_invoke_all('user_logout', $user);
    // Destroy the current session, and reset $user to the anonymous user.
    session_destroy();
    drupal_session_start();
    drupal_set_message(t('Logged current user out.'), 'info');
  }
  if ($user->uid == 0) {
    // Unauthenticated so create user if necessary.
    if ($account = user_load_by_name($lti_user)) {
      // User exists.
      if ($account->uid == 1) {
        // User 1 must use drupal authentication.
        drupal_set_message(t('Admin account must use Drupal authentication.'), 'error');
        return;
      }
    }
    else {
      // Provision a user account for $lti_user.
      if (!isset($launch_info['lis_person_contact_email_primary'])) {
        if ($domain == '') {
          $email = $lti_user . '@example.com';
        }
        else {
          $email = $lti_user . '.example.com';
        }
      }
      else {
        $email = $launch_info['lis_person_contact_email_primary'];
      }
      $user_attribute_settings = variable_get('lti_tool_provider_user_attribute_mapping', array());
      $fields = lti_tool_provider_retrieve_user_field_types('TEXT');
      $userinfo = array(
        'name' => $lti_user,
        'pass' => user_password(20),
        'init' => $email,
        'mail' => $email,
        'status' => 1,
        'access' => REQUEST_TIME,
      );
      foreach ($user_attribute_settings as $variable => $field) {
        if (($field != 'none') && isset($fields[$field]) && isset($launch_info[$variable])) {
          $userinfo[$field] = array(LANGUAGE_NONE => array('0' => array('value' => $launch_info[$variable])));
        }
      }
      if (!$account = user_save(drupal_anonymous_user(), $userinfo)) {
        drupal_set_message(t('User account creation failed because of system problems.'), 'error');
        return;
      }
      drupal_set_message(t('User account created.'), 'info');
    }
  }
  else {
    $account = user_load($user->uid);
  }
  // $account is the $lti_user.
  // Map Drupal global roles based on the user LTI role.
  if (isset($launch_info['roles'])) {
    lti_tool_provider_assign_global_roles_to_user($launch_info['roles'], $account->uid);
  }
  $destination = '';
  // If unauthenticated, log user in.
  if ($user->uid == 0) {
    $form_state = array('uid' => $account->uid);
    user_login_submit(array(), $form_state);
  }
  // Map Group roles based on the user LTI role.
  if (module_exists('og')) {
    $selected_group_bundle = variable_get('lti_tool_provider_group_mapping_bundle');
    if ($selected_group_bundle != 'none:None') {
      // Grant Group Membership and Group Role.
      $users_groups = og_get_entity_groups('user', $account->uid);
      // Find the group that matches the lti context_label.
      $course_found = FALSE;
      list($group_entity, $group_bundle) = explode(':', variable_get('lti_tool_provider_group_mapping_bundle'));
      $saved_settings = variable_get('lti_tool_provider_group_mapping', array());
      $mapping_field = $saved_settings['context_id'];
      foreach (og_get_all_group($group_entity) as $key => $group_id) {
        $group_wrapper = entity_metadata_wrapper($group_entity, $group_id);
        $fields = $group_wrapper->getPropertyInfo();
        if (isset($fields[$mapping_field]) && $group_wrapper->{$mapping_field}->value() == $launch_info['context_id']) {
          $course_found = TRUE;
          $launch_info['course_entity_type'] = $group_entity;
          $launch_info['course_entity_eid'] = $group_id;
          // Found the group that matches the context_id.
          // Add the user to the group.
          if (!array_key_exists($group_entity, $users_groups) || !in_array($group_id, $users_groups[$group_entity])) {
            og_group($group_entity, $group_id, array(
              'entity_type' => 'user',
              'entity' => $account->uid,
              'field_name' => FALSE,
              'state' => OG_STATE_ACTIVE,
            ));
          }
          if (isset($launch_info['roles'])) {
            $found_roles = lti_tool_provider_search_roles($launch_info['roles']);
            foreach ($found_roles as $lti_role => $found_role) {
              og_role_grant($group_entity, $group_id, $account->uid, $found_role);
            }
          }
        }
      }
      // Provision group.
      // FIX THIS does not cope with non-node group bundles.
      if (!$course_found && ($launch_info['context_id'] != NULL) && (variable_get('lti_tool_provider_provision_groups') == 1) && (user_access('create ' . $group_bundle . ' content', $account))) {
        $node = new stdClass();
        $node->type = $group_bundle;
        $node->language = LANGUAGE_NONE;
        node_object_prepare($node);
        $saved_settings = variable_get('lti_tool_provider_group_mapping', array());
        foreach ($saved_settings as $variable => $field) {
          if ($field != 'none') {
            if ($field != 'title') {
              foreach (field_info_instances($group_entity, $group_bundle) as $entity_field) {
                if (($field == $entity_field['field_name']) && (strcasecmp($entity_field['widget']['module'], 'text') == 0)) {
                  $node->{$field}[LANGUAGE_NONE][0]['value'] = $launch_info[$variable];
                }
              }
            }
            else {
              $node->$field = $launch_info[$variable];
            }
          }
        }
        node_save($node);
        $group_nid = $node->nid;
        $launch_info['course_entity_type'] = $group_entity;
        $launch_info['course_entity_eid'] = $group_nid;
      }
      if (isset($launch_info['course_entity_eid'])) {
        $destination = $launch_info['course_entity_type'] . '/' . $launch_info['course_entity_eid'];
      }
      else {
        drupal_set_message(check_plain('No course corresponding to ' . $launch_info['context_label'] . ' exists.'));
      }
    }
  }
  // Save launch information in session.
  $_SESSION['lti_tool_provider_context_info'] = $launch_info;
  // Go to the required destination.
  if (key_exists('custom_destination', $launch_info)) {
    $destination .= '/' . $launch_info['custom_destination'];
  }
  module_invoke_all('lti_tool_provider_launch', $launch_info);
  lti_tool_provider_goto($destination);
  return 'Launched';
}

/**
 * Menu title callback for the lti return menu item.
 */
function lti_tool_provider_return_title($title) {
  if (isset($_SESSION['lti_tool_provider_context_info'])) {
    if (key_exists('custom_return_label', $_SESSION['lti_tool_provider_context_info'])) {
      return $_SESSION['lti_tool_provider_context_info']['custom_return_label'];
    }
    return 'Return to LMS';
  }
}

/**
 * Page callback for the lti return menu item.
 *
 * @return string
 *   The page content if any.
 */
function lti_tool_provider_return() {
  // Logout the user and redirect to the return url in the ltitp_context_info.
  global $user;
  // If in a lti context log user out and return to launch presentation.
  // otherwise just logout.
  if (isset($_SESSION['lti_tool_provider_context_info'])) {
    module_invoke_all('lti_tool_provider_return');
    $url = $_SESSION['lti_tool_provider_context_info']['launch_presentation_return_url'];
    watchdog('user', 'Session closed for %name.', array('%name' => $user->name));
    module_invoke_all('user_logout', $user);
    // Destroy the current session, and reset $user to the anonymous user.
    session_destroy();
    lti_tool_provider_goto($url);
  }
  else {
    lti_tool_provider_goto('user/logout');
  }
  return '';
}

/**
 * Menu title callback for the LTI Home menu item.
 *
 * @return string
 *   The title.
 */
function lti_tool_provider_home_title($title) {
  if (isset($_SESSION['lti_tool_provider_context_info'])) {
    if (key_exists('custom_destination_label', $_SESSION['lti_tool_provider_context_info'])) {
      return $_SESSION['lti_tool_provider_context_info']['custom_destination_label'];
    }
    else {
      if (key_exists('resource_link_title', $_SESSION['lti_tool_provider_context_info'])) {
        return $_SESSION['lti_tool_provider_context_info']['resource_link_title'];
      }
    }
  }
  return 'Home';
}

/**
 * Menu page callback for the LTI Home menu item.
 */
function lti_tool_provider_home() {
  // If in a lti context return to group home page + custom destination.
  // Otherwise just the site home.
  $destination = '';
  if (isset($_SESSION['lti_tool_provider_context_info'])) {
    if (key_exists('course_entity_type', $_SESSION['lti_tool_provider_context_info'])) {
      $destination = $_SESSION['lti_tool_provider_context_info']['course_entity_type'] . '/' . $_SESSION['lti_tool_provider_context_info']['course_entity_eid'];
    }
    if (key_exists('custom_destination', $_SESSION['lti_tool_provider_context_info'])) {
      $destination .= '/' . $_SESSION['lti_tool_provider_context_info']['custom_destination'];
    }
  }
  drupal_goto($destination);
}

/**
 * Menu page callback for the LTI Info menu items.
 *
 * @return array
 *   The conent for the page.
 */
function lti_tool_provider_info() {
  // Display all the key/value pairs in the ltitp_context_info.
  $content = array();
  $content[] = array(
    '#type' => 'item',
    '#markup' => t('LTI Context Session Variables'),
  );
  if (isset($_SESSION['lti_tool_provider_context_info'])) {
    $info = $_SESSION['lti_tool_provider_context_info'];
    $rows = array();
    $loop_counter = 0;
    foreach ($info as $key => $value) {
      $rows[$loop_counter]['data']['key'] = $key;
      $rows[$loop_counter]['data']['value'] = $value;
      $loop_counter++;
    }
    $content['table'] = array(
      '#theme' => 'table',
      '#rows' => $rows,
      '#header' => array(t('Key'), t('Value')),
    );
  }
  else {
    $content['table'] = array(
      '#type' => 'item',
      '#markup' => t('There is no LTI Context'),
    );
  }
  return $content;
}

/**
 * Menu page callback.
 *
 * Configure settings for mapping user attributes from the LTI context variables.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_user_attributes($form, &$form_state) {
  $form['description'] = array(
    '#type' => 'item',
    '#title' => t('LTI Context to User Attribute Mapping'),
    '#description' => 'Choose the user attributes to be mapped from each LTI context variable',
  );
  $fields_array = lti_tool_provider_retrieve_user_field_types('TEXT');
  $lis_list = lti_tool_provider_user_mapping_lis_details();
  $saved_settings = variable_get('lti_tool_provider_user_attribute_mapping', array());
  $form['mapping'] = array(
    '#tree' => TRUE,
    '#theme' => 'table',
    '#header' => array(t('LTI Context Variable'), t('User Attribute')),
    '#rows' => array(),
  );
  foreach ($lis_list as $detail) {
    $variable = array(
      '#type' => 'item',
      '#title' => check_plain($detail),
    );
    $value = $saved_settings[$detail];
    $attribute = array(
      '#type' => 'select',
      '#options' => $fields_array,
      '#default_value' => $value,
    );
    $form['mapping'][] = array(
      'variable' => &$variable,
      'attribute' => &$attribute,
    );
    $form['mapping']['#rows'][] = array(
      array('data' => &$variable),
      array('data' => &$attribute),
    );
    unset($variable);
    unset($attribute);
  }
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Save User Attributes'),
    '#submit' => array('lti_tool_provider_user_attributes_submit'),
  );
  return $form;
}

/**
 * Validation for the user attributes form.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_user_attributes_validate($form, &$form_state) {
  $check = TRUE;
  $lis_list = lti_tool_provider_user_mapping_lis_details();
  $field_options = lti_tool_provider_retrieve_user_field_types('TEXT');
  $counters = array();
  foreach ($field_options as $field => $desc) {
    $counters[$field] = 0;
  }
  foreach ($lis_list as $key => $variable) {
    $counters[$form_state['values']['mapping'][$key]['attribute']]++;
  }
  foreach ($field_options as $field => $desc) {
    if ($field != 'none' && $counters[$field] > 1) {
      $check = FALSE;
      break;
    }
  }
  if (!$check) {
    form_set_error('', t('You may not map multiple values to the same attribute.'));
  }
}

/**
 * Submit handler for the user attributes form.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_user_attributes_submit($form, &$form_state) {
  $lis_list = lti_tool_provider_user_mapping_lis_details();
  $settings = array();
  foreach ($lis_list as $key => $lis) {
    $setting[$lis] = $form_state['values']['mapping'][$key]['attribute'];
  }
  variable_set('lti_tool_provider_user_attribute_mapping', $setting);
  drupal_set_message(t('User attribute mapping saved.'));
}

/**
 * Admin form for the global roles.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_global_roles($form, &$form_state) {
  $form['description'] = array(
    '#type' => 'item',
    '#title' => t('Map LTI Roles to Global Drupal Roles'),
    '#description' => t('Map each LTI role to a global Drupal role.'),
  );
  $select_roles = user_roles(TRUE, NULL);
  $old_role_array = variable_get('lti_tool_provider_global_role_array');
  $form['roles'] = array(
    '#tree' => TRUE,
    '#theme' => 'table',
    '#header' => array(t('LTI Roles'), t('Global Roles')),
    '#rows' => array(),
  );
  foreach (lti_tool_provider_get_lti_roles() as $role) {
    $lti_role = array(
      '#type' => 'item',
      '#title' => check_plain($role),
    );
    $global_role = array(
      '#type' => 'select',
      '#options' => $select_roles,
    );
    if (isset($old_role_array[$role]) && isset($select_roles[$old_role_array[$role]])) {
      $global_role['#default_value'] = $old_role_array[$role];
    }
    else {
      $global_role['#default_value'] = array_search(DRUPAL_AUTHENTICATED_RID, $select_roles);
    }
    $form['roles'][] = array(
      'lti_role' => &$lti_role,
      'global_role' => &$global_role,
    );
    $form['roles']['#rows'][] = array(
      array('data' => &$lti_role),
      array('data' => &$global_role),
    );
    unset($lti_role);
    unset($global_role);
  }
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Save Global Roles'),
    '#submit' => array('lti_tool_provider_global_roles_submit'),
  );
  return $form;
}

/**
 * Submit handler for the global roles form.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_global_roles_submit($form, &$form_state) {
  $settings = array();
  foreach (lti_tool_provider_get_lti_roles() as $key => $role) {
    $settings[$role] = $form_state['values']['roles'][$key]['global_role'];
  }
  variable_set('lti_tool_provider_global_role_array', $settings);
  drupal_set_message(t('LTI global roles mapping saved.'));
}

/**
 * Admin form for the group mapping.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_group_mapping($form, &$form_state) {
  $form = array();
  $form['description'] = array(
    '#type' => 'item',
    '#title' => t('Choose a group bundle, configure LTI context variable to group attribute mapping and group role mapping.'),
    '#weight' => -8,
  );
  $new_bundles = lti_tool_provider_get_group_bundles();
  $old_bundle = variable_get('lti_tool_provider_group_mapping_bundle');
  $form['bundle'] = array(
    '#type' => 'select',
    '#title' => t('LTI Context to Drupal group mapping'),
    '#description' => 'Please select the Drupal group bundle corresponding to an LTI course.',
    '#options' => $new_bundles,
    '#weight' => -7,
    '#ajax' => array(
      'callback' => 'lti_tool_provider_group_mapping_select_callback',
      'wrapper' => 'group-detail',
      'method' => 'replace',
      'effect' => 'fade',
    ),
  );
  // The group_detail section of the form is ajax reloaded when the bundle
  // or the creat_course checkbox changes.
  $form['bundle']['#default_value'] = isset($form_state['values']['bundle']) ? $form_state['values']['bundle'] : $old_bundle;
  $is_node_type = (reset(explode(':', $form['bundle']['#default_value'])) == 'node') ? TRUE : FALSE;
  $form['group_detail'] = array(
    '#prefix' => '<div id="group-detail">',
    '#suffix' => '</div>',
    '#weight' => -6,
  );
  if (!empty($form['bundle']['#default_value']) && $form['bundle']['#default_value'] != 'none:None') {
    if ($is_node_type) {
      $form['group_detail']['create_course'] = array(
        '#type' => 'checkbox',
        '#title' => t('Automatically create groups. Note: Only available for node group bundles and LTI user must have appropriate "Create content type" permission.'),
        '#default_value' => isset($form_state['values']['create_course']) ? $form_state['values']['create_course'] : variable_get('lti_tool_provider_provision_groups'),
        '#weight' => -5,
        '#ajax' => array(
          'callback' => 'lti_tool_provider_group_mapping_select_callback',
          'wrapper' => 'group-detail',
          'method' => 'replace',
          'effect' => 'fade',
        ),
      );
    }
    $form['group_detail']['mapping_description'] = array(
      '#prefix' => '<div id="select-mapping-div">',
      '#suffix' => '</div>',
      '#type' => 'item',
      '#title' => t('LTI group attribute mapping'),
      '#description' => 'Choose the group attributes to be mapped from the LTI context variables. Note: Title and context_id must be mapped but not necessarily to each other.',
      '#weight' => -4,
    );
    if ($is_node_type && $form['group_detail']['create_course']['#default_value'] == 0) {
      $form['group_detail']['mapping_description']['#description'] = 'Choose the group attribute to be mapped from the LTI context_id.';
    }
    $saved_settings = variable_get('lti_tool_provider_group_mapping', array());
    $form['group_detail']['mapping'] = array(
      '#tree' => TRUE,
      '#theme' => 'table',
      '#header' => array(t('LTI Context Variable'), t('Group Attribute')),
      '#rows' => array(),
      '#weight' => -1,
    );
    foreach (lti_tool_provider_context_mapping_details() as $key => $detail) {
      if ((!$is_node_type || $form['group_detail']['create_course']['#default_value'] == 0) && ($detail != 'context_id')) {
        unset($form_state['values']['mapping'][$key]);
      }
      else {
        $variable = array(
          '#type' => 'item',
          '#title' => check_plain($detail),
        );
        $attribute = array(
          '#type' => 'select',
          '#options' => lti_tool_provider_retrieve_entity_field_types($form['bundle']['#default_value']),
        );
        if (isset($form_state['values'][$detail])) {
          $attribute['#default_value'] = $form_state['values'][$detail];
        }
        else {
          $attribute['#default_value'] = isset($saved_settings[$detail]) ? $saved_settings[$detail] : 0;
        }
        $form['group_detail']['mapping'][] = array(
          'variable' => &$variable,
          'attribute' => &$attribute,
        );
        $form['group_detail']['mapping']['#rows'][] = array(
          array('data' => &$variable),
          array('data' => &$attribute),
        );
        unset($variable);
        unset($attribute);
      }
    }
    $form['group_detail']['mapping']['#weight'] = -3;
    if ($form['bundle']['#default_value'] != 'none:None') {
      $form['group_detail']['roles_description'] = array(
        '#type' => 'item',
        '#title' => t('LTI group role mapping'),
        '#description' => 'Choose the group roles to be mapped from the LTI roles.',
        '#weight' => -2,
      );
      list($entity, $bundle) = explode(':', $form['bundle']['#default_value']);
      $select_roles = og_roles($entity, $bundle);
      // Remove OG_ANONYMOUS_ROLE in $select_roles.
      if (($key = array_search(OG_ANONYMOUS_ROLE, $select_roles)) !== FALSE) {
        unset($select_roles[$key]);
      }
      $old_bundle = variable_get('lti_tool_provider_group_mapping_bundle');
      if ($old_bundle == $form['bundle']['#default_value']) {
        $old_role_array = variable_get('lti_tool_provider_group_role_array');
      }

      $form['group_detail']['roles'] = array(
        '#tree' => TRUE,
        '#theme' => 'table',
        '#header' => array(t('LTI Roles'), t('Group Roles')),
        '#rows' => array(),
        '#weight' => -1,
      );
      foreach (lti_tool_provider_get_lti_roles() as $role) {
        $lti_role = array(
          '#type' => 'item',
          '#title' => check_plain($role),
        );
        $group_role = array(
          '#type' => 'select',
          '#options' => $select_roles,
        );
        if (isset($old_role_array[$role]) && isset($select_roles[$old_role_array[$role]])) {
          $group_role['#default_value'] = $old_role_array[$role];
        }
        else {
          $group_role['#default_value'] = array_search(DRUPAL_AUTHENTICATED_RID, $select_roles);
        }
        $form['group_detail']['roles'][] = array(
          'lti_role' => &$lti_role,
          'group_role' => &$group_role,
        );
        $form['group_detail']['roles']['#rows'][] = array(
          array('data' => &$lti_role),
          array('data' => &$group_role),
        );
        unset($lti_role);
        unset($group_role);
      }
    }
  }
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => 'Save Group Mapping',
    '#submit' => array('lti_tool_provider_group_mapping_submit'),
    '#weight' => 10,
  );
  return $form;
}

/**
 * AJAX callback for the group mapping form.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_group_mapping_select_callback($form, &$form_state) {
  return $form['group_detail'];
}

/**
 * Validation callback.
 * 
 * @param unknown $form
 * @param unknown $form_state
 */
function lti_tool_provider_group_mapping_validate($form, &$form_state) {
  $entity_bundle = $form_state['values']['bundle'];
  list($entity, $bundle) = explode(':', $entity_bundle);
  if ($entity == 'none') {
    return;
  }
  $check_isset_title = FALSE;
  $check_isset_context_id = FALSE;
  $check_repeat = FALSE;
  $fields = lti_tool_provider_retrieve_entity_field_types($entity_bundle);
  $counters = array();
  foreach (lti_tool_provider_context_mapping_details() as $key => $detail) {
    if (isset($form_state['values']['mapping'][$key]['attribute'])) {
      if ($detail != 'context_type' && $form_state['input']['mapping'][$key]['attribute'] == 'title') {
        $check_isset_title = TRUE;
      }
      if ($detail == 'context_id' && $form_state['input']['mapping'][$key]['attribute'] != 'none') {
        $check_isset_context_id = TRUE;
      }
      $value = $form_state['input']['mapping'][$key]['attribute'];
      isset($counters[$value]) ? $counters[$value]++  : 0;
    }
  }
  foreach ($counters as $counter => $count) {
    if ($counter != 'none' && $count > 1) {
      $check_repeat = TRUE;
      break;
    }
  }
  if (!$check_isset_context_id) {
    form_set_error('group_detail', t('You must map context_id variable to a field.'));
  }
  if ($check_repeat) {
    form_set_error('group_detail', t('You may not map more than one variable to the same field.'));
  }
  if ($entity == 'node' && isset($form_state['values']['create_course']) && $form_state['values']['create_course'] == 1 && !$check_isset_title) {
    form_set_error('group_detail', t('Title must be mapped from one of context_id, context_label or context_title.'));
  }
  return;
}

/**
 * Update or create a consumer entity.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_group_mapping_submit($form, &$form_state) {
  variable_set('lti_tool_provider_group_mapping_bundle', $form_state['values']['bundle']);
  if ($form_state['values']['bundle'] == 'none:None') {
    variable_set('lti_tool_provider_provision_groups', 0);
  }
  else {
    variable_set('lti_tool_provider_provision_groups', $form_state['values']['create_course']);
    $settings = array();
    foreach (lti_tool_provider_context_mapping_details() as $key => $detail) {
      if (isset($form_state['input']['mapping'][$key]['attribute'])) {
        $settings[$detail] = $form_state['input']['mapping'][$key]['attribute'];
      }
    }
    variable_set('lti_tool_provider_group_mapping', $settings);
    $group_roles = array();
    foreach (lti_tool_provider_get_lti_roles() as $key => $role) {
      $group_roles[$role] = $form_state['input']['roles'][$key]['group_role'];
    }
    variable_set('lti_tool_provider_group_role_array', $group_roles);
  }
  drupal_set_message(t('LTI group mapping saved.'));
}

/**
 * Generate the consumer admin form.
 *
 * @return array
 *   The form array.
 */
function lti_tool_provider_consumers_admin() {
  $content = array();
  $content[] = array(
    '#type' => 'item',
    '#markup' => t('Administration page for LTI Tool Consumers.'),
  );
  $content[] = array(
    '#type' => 'item',
    '#markup' => l(t('Add a new LTI Tool Consumer'), 'admin/config/lti-tool-provider/lti-tool-consumer/add'),
  );
  $content['table'] = array();
  // Load all of our entities.
  $entities = lti_tool_provider_consumer_load_multiple();
  $entity_type = 'lti_tool_provider_consumer';
  if (!empty($entities)) {
    $content['entity_table'] = array(
      '#theme' => 'table',
      '#rows' => array(),
      '#header' => array(t('ID'), t('Consumer'), t('Key'), t('Secret'), t('Domain'), t('Dummy Account'), t('Date Joined')),
    );
    $loop_counter = 0;
    foreach ($entities as $entity) {
      // Create tabular rows for our entities.
      $content['entity_table']['#rows'][$loop_counter] = array(
        'data' => array(
          'id' => $entity->lti_tool_provider_consumer_id,
          // This statement can create url to view individual keys.
          'Consumer' => l($entity->lti_tool_provider_consumer_consumer, 'admin/config/lti-tool-provider/lti-tool-consumer/' . $entity->lti_tool_provider_consumer_id),
          'Key' => $entity->lti_tool_provider_consumer_key,
          'Secret' => $entity->lti_tool_provider_consumer_secret,
          'Domain' => $entity->lti_tool_provider_consumer_domain,
          'Dummy_Pref' => lti_tool_provider_trans_bool($entity->lti_tool_provider_consumer_dummy_pref),
          'Date Joined' => format_date($entity->date_joined),
        ),
      );
      $extra_cols = lti_tool_provider_get_extra_columns();
      if (isset($extra_cols)) {
        foreach ($extra_cols as $col) {
          if (!isset($entity->$col)) {
            $value = 'Not avaliable';

          }
          else {
            $value = $entity->$col;
          }
          $content['entity_table']['#rows'][$loop_counter]['data'][lti_tool_provider_get_column_desc($entity_type, $col)] = $value;
        }
      }
      $loop_counter++;
    }
    if (isset($extra_cols)) {
      foreach ($extra_cols as $col) {
        $content['entity_table']['#header'][] = t(lti_tool_provider_get_column_desc($entity_type, $col));
      }
    }
  }
  else {
    // There were no entities. Tell the user.
    $content['entity_table'][] = array(
      '#type' => 'item',
      '#markup' => t('No LTITP Consumer entities currently exist.'),
    );
  }
  return $content;
}

/**
 * Generate the consumer entity form.
 *
 * @param array $form
 * @param array $form_state
 * @param object $entity
 *   The consumer entity to display.
 * @return array
 *   The form array.
 */
function lti_tool_provider_consumer_form($form, &$form_state, $entity) {
  $form['lti_tool_provider_consumer_consumer'] = array(
    '#type' => 'textfield',
    '#title' => t('Consumer'),
    '#required' => TRUE,
    '#default_value' => $entity->lti_tool_provider_consumer_consumer,
  );
  $form['lti_tool_provider_consumer_key'] = array(
    '#type' => 'textfield',
    '#title' => t('Key'),
    '#required' => TRUE,
    '#default_value' => $entity->lti_tool_provider_consumer_key,
  );
  $form['lti_tool_provider_consumer_secret'] = array(
    '#type' => 'textfield',
    '#title' => t('Secret'),
    '#required' => TRUE,
    '#default_value' => $entity->lti_tool_provider_consumer_secret,
  );
  $form['lti_tool_provider_consumer_domain'] = array(
    '#type' => 'textfield',
    '#title' => t('Domain'),
    '#required' => FALSE,
    '#default_value' => $entity->lti_tool_provider_consumer_domain,
  );
  $form['lti_tool_provider_consumer_dummy_pref'] = array(
    '#type' => 'checkbox',
    '#title' => t('Use a dummy account for unidentified users'),
    '#default_value' => $entity->lti_tool_provider_consumer_dummy_pref,
  );
  $form['lti_tool_provider_consumer_id'] = array(
    '#type' => 'value',
    '#value' => $entity->lti_tool_provider_consumer_id,
  );
  $form['lti_tool_provider_consumer'] = array(
    '#type' => 'value',
    '#value' => $entity,
  );
  field_attach_form('lti_tool_provider_consumer', $entity, $form, $form_state);
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Save'),
    '#submit' => array('lti_tool_provider_consumer_form_submit'),
    '#weight' => 100,
  );
  if (current_path() != "lti-tool-consumer/add") {
    $form['delete'] = array(
      '#type' => 'submit',
      '#value' => t('Delete'),
      '#submit' => array('lti_tool_provider_consumer_edit_delete'),
      '#weight' => 200,
    );
  }
  return $form;
}

/**
 * Validate the consumer entity form.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_consumer_form_validate($form, &$form_state) {
  $check_key = FALSE;
  $check_domain = FALSE;
  $new_consumer = $form_state['values']['lti_tool_provider_consumer_id'];
  if ($new_consumer == 0) {
    // Create.
    $query = new EntityFieldQuery();
    $query
    ->entityCondition('entity_type', 'lti_tool_provider_consumer')
    ->propertyCondition('lti_tool_provider_consumer_key', $form_state['values']['lti_tool_provider_consumer_key']);
    $result = $query->execute();
    if (empty($result)) {
      $check_key = TRUE;
    }
    if ($form_state['values']['lti_tool_provider_consumer_domain'] == '') {
      $check_domain = TRUE;
    }
    else {
      $query = new EntityFieldQuery();
      $query
      ->entityCondition('entity_type', 'lti_tool_provider_consumer')
      ->propertyCondition('lti_tool_provider_consumer_domain', $form_state['values']['lti_tool_provider_consumer_domain']);
      $result = $query->execute();
      if (empty($result)) {
        $check_domain = TRUE;
      }
    }
  }
  else {
    // Edit or delete.
    $query = new EntityFieldQuery();
    $query
    ->entityCondition('entity_type', 'lti_tool_provider_consumer')
    ->propertyCondition('lti_tool_provider_consumer_key', $form_state['values']['lti_tool_provider_consumer_key'])
    ->propertyCondition('lti_tool_provider_consumer_id', $form_state['values']['lti_tool_provider_consumer_id'], '!=');
    $result = $query->execute();
    if (empty($result)) {
      $check_key = TRUE;
    }
    $query = new EntityFieldQuery();
    $query
    ->entityCondition('entity_type', 'lti_tool_provider_consumer')
    ->propertyCondition('lti_tool_provider_consumer_domain', $form_state['values']['lti_tool_provider_consumer_domain'])
    ->propertyCondition('lti_tool_provider_consumer_id', $form_state['values']['lti_tool_provider_consumer_id'], '!=');
    $result = $query->execute();
    if (empty($result)) {
      $check_domain = TRUE;
    }
  }
  if ($check_key && $check_domain) {
    $submitted = (object) $form_state['values']['lti_tool_provider_consumer'];
    field_attach_form_validate('lti_tool_provider_consumer', $submitted, $form, $form_state);
  }
  elseif (!$check_key && !$check_domain) {
    form_set_error('', t('Keys and domains must be unique.'));
  }
  elseif (!$check_key) {
    form_set_error('', t('Keys must be unique.'));
  }
  elseif (!$check_domain) {
    form_set_error('', t('Domains must be unique.'));
  }
  $pattern = '/([A-Z0-9 _!@#$%=+*.?<>\/\r\n])/';
  $domain_char_check = preg_match($pattern, $form_state['values']['lti_tool_provider_consumer_domain']);
  if ($domain_char_check) {
    form_set_error('', t('The domain must not contain numeric value, special characters, spaces and upper cases'));
  }
}

/**
 * Update or create a consumer entity.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_consumer_form_submit($form, &$form_state) {
  $entity = $form_state['values']['lti_tool_provider_consumer'];
  $entity->lti_tool_provider_consumer_key = $form_state['values']['lti_tool_provider_consumer_key'];
  $entity->lti_tool_provider_consumer_secret = $form_state['values']['lti_tool_provider_consumer_secret'];
  $entity->lti_tool_provider_consumer_consumer = $form_state['values']['lti_tool_provider_consumer_consumer'];
  $entity->lti_tool_provider_consumer_domain = $form_state['values']['lti_tool_provider_consumer_domain'];
  $entity->lti_tool_provider_consumer_dummy_pref = $form_state['values']['lti_tool_provider_consumer_dummy_pref'];
  if ($form_state['values']['lti_tool_provider_consumer_dummy_pref']) {
    lti_tool_provider_create_dummy_account($form_state['values']['lti_tool_provider_consumer_domain']);
    drupal_set_message(t('Dummy user has been created.'));
  }
  else {
    if ($form_state['values']['lti_tool_provider_consumer_id'] != 0) {
      lti_tool_provider_remove_dummy_account($form_state['values']['lti_tool_provider_consumer_domain']);
      drupal_set_message(t('Dummy user has been removed.'));
    }
  }
  field_attach_submit('lti_tool_provider_consumer', $entity, $form, $form_state);
  $entity = lti_tool_provider_consumer_save($entity);
  drupal_set_message(t('Configurations saved.'));
  $form_state['redirect'] = 'admin/config/lti-tool-provider/lti-tool-consumers';
}

/**
 * Edit or delete a consumer entity.
 *
 * @param array $form
 * @param array $form_state
 */
function lti_tool_provider_consumer_edit_delete($form , &$form_state) {
  $entity = $form_state['values']['lti_tool_provider_consumer'];
  lti_tool_provider_remove_dummy_account($entity->lti_tool_provider_consumer_domain);
  lti_tool_provider_consumer_delete($entity);
  drupal_set_message(t('The entity %lti_tool_provider_consumer_consumer (ID %lti_tool_provider_consumer_id) has been deleted.', array(
    '%lti_tool_provider_consumer_consumer' => $entity->lti_tool_provider_consumer_consumer,
    '%lti_tool_provider_consumer_id' => $entity->lti_tool_provider_consumer_id,
  )));
  $form_state['redirect'] = 'admin/config/lti-tool-provider/lti-tool-consumers';
}

/**
 * View a single consumer entity.
 *
 * @param object $entity
 *   The consumer to view.
 * @param string $view_mode
 * @return array
 *   The content array from the entity.
 */
function lti_tool_provider_consumer_view($entity, $view_mode = 'tweaky') {
  $entity_type = 'lti_tool_provider_consumer';
  $entity->content = array(
    '#view_mode' => $view_mode,
  );
  field_attach_prepare_view($entity_type, array($entity->lti_tool_provider_consumer_id => $entity), $view_mode);
  entity_prepare_view($entity_type, array($entity->lti_tool_provider_consumer_id => $entity));
  $entity->content += field_attach_view($entity_type, $entity, $view_mode);
  $entity->content['lti_tool_provider_consumer_consumer'] = array(
    '#type' => 'item',
    '#title' => t('Consumer'),
    '#markup' => $entity->lti_tool_provider_consumer_consumer,
  );
  $entity->content['lti_tool_provider_consumer_key'] = array(
    '#type' => 'item',
    '#title' => t('Key'),
    '#markup' => $entity->lti_tool_provider_consumer_key,
  );
  $entity->content['lti_tool_provider_consumer_secret'] = array(
    '#type' => 'item',
    '#title' => t('Secret'),
    '#markup' => $entity->lti_tool_provider_consumer_secret,
  );
  $entity->content['lti_tool_provider_consumer_domain'] = array(
    '#type' => 'item',
    '#title' => t('Domain'),
    '#markup' => $entity->lti_tool_provider_consumer_domain,
  );
  $entity_type = 'lti_tool_provider_consumer';
  $extra_cols = lti_tool_provider_get_extra_columns($entity_type);
  if (isset($extra_cols)) {
    foreach ($extra_cols as $col) {
      if (!isset($entity->$col)) {
        $value = 'Not avaliable';
      }
      else {
        $value = $entity->$col;
      }
      $entity->content[$col] = array(
        '#type' => 'item',
        '#title' => t(lti_tool_provider_get_column_desc($entity_type, $col)),
        '#markup' => $value,
      );
    }
  }
  $entity->content['lti_tool_provider_consumer_dummy_pref'] = array(
    '#type' => 'item',
    '#title' => t('Activate Dummy Account'),
    '#markup' => lti_tool_provider_trans_bool($entity->lti_tool_provider_consumer_dummy_pref),
  );
  $entity->content['date_joined'] = array(
    '#type' => 'item',
    '#title' => t('Date Joined'),
    '#markup' => format_date($entity->date_joined),
  );
  global $language;
  $langcode = $language->language;
  $entity->content['#language'] = $langcode;
  module_invoke_all('entity_view', $entity, $entity_type, $view_mode, $langcode);
  drupal_alter(array('lti_tool_provider_consumer_view', 'entity_view'),
    $entity->content, $entity_type);
  return $entity->content;
}