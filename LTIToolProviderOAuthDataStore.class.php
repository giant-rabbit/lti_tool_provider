<?php
/**
 * @file
 * Contains LTIToolProviderOAuthDataStore.
 */

$library = libraries_load('oauth');
if ($library['loaded'] == FALSE) {
  drupal_set_message(t('Missing OAuth library: @error', array('@error' => $library['error'])), 'error');
}
else {
  require_once($library['library path'] . '/OAuth.php');

  class LTIToolProviderOAuthDataStore extends OAuthDataStore {

    /**
     * Find a consumer matching a key.
     *
     * @param string $consumer_key
     *
     * @return OAuthConsumer
     */
    function lookup_consumer($consumer_key) {
      $query = new EntityFieldQuery();
      $result = $query->entityCondition('entity_type', 'lti_tool_provider_consumer')
      ->propertyCondition('lti_tool_provider_consumer_key', $consumer_key, '=')
      ->addMetaData('account', user_load(1))
      ->execute();
      if (isset($result['lti_tool_provider_consumer'])) {
        $consumer_ids = array_keys($result['lti_tool_provider_consumer']);
        $consumers = entity_load('lti_tool_provider_consumer', $consumer_ids);
        $consumer_entity = reset($consumers);
        $consumer = new OAuthConsumer($consumer_key, $consumer_entity->lti_tool_provider_consumer_secret, NULL);
      }
      else {
        $consumer = new OAuthConsumer($consumer_key, NULL, NULL);
      }
      return $consumer;
    }

    /**
     * Lookup a token.
     *
     * @param OAuthConsumer $consumer
     * @param string $token_type
     * @param OAuthToken $token
     *
     * @return OAuthToken
     */
    function lookup_token($consumer, $token_type, $token) {
      return new OAuthToken($consumer, '');
    }

    /**
     * Lookup a nonce.
     *
     * @param OAuthConsumer $consumer
     * @param OAuthToken $token
     * @param string $nonce
     * @param integer $timestamp
     *
     * @return NULL
     */
    function lookup_nonce($consumer, $token, $nonce, $timestamp) {
      return NULL;
    }

    /**
     * Get a new request token.
     *
     * @param OAuthConsumer $consumer
     * @param string $callback
     *
     * @return NULL
     */
    function new_request_token($consumer, $callback = NULL) {
      // Return a new token attached to this consumer.
      return NULL;
    }

    /**
     * Get a new access token.
     *
     * @param OAuthToken $token
     * @param OAuthConsumer $consumer
     * @param string $verifier
     *
     * @return NULL
     */
    function new_access_token($token, $consumer, $verifier = NULL) {
      // Return a new access token attached to this consumer
      // for the user associated with this token if the request token
      // is authorized.
      // Should also invalidate the request token.
      return NULL;
    }
  }
}
